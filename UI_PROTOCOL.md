# UI protocol

Measured reference for every screen in the desktop client: real sizes, real
positions, and the exact file and line where each element, its stylesheet rule
and its tokens are defined.

Sizes and positions come from walking a live Qt widget tree, so they are a
snapshot. Re-measure after changing layout, tokens or the stylesheet.

## How to trace any element

Each element resolves through four hops:

```
widget class      -> mikai_ui/<module>.py:<line>      the code that builds it
objectName        -> mikai_ui/theme/__init__.py:<line> the QSS rule that styles it
size / position   -> measured, driven by tokens below
tokens            -> mikai_ui/theme/{colors,metrics}.py:<line>
```

An element with no `objectName` is unstyled: it inherits the base `QWidget`
rule and is laid out purely by its parent layout.

## Tokens

Every size and colour in the UI comes from these two files. No raw values in
view code.

### Colours

| token | value | defined at |
|---|---|---|
| `BACKGROUND` | `"#0b0b0f"` | `mikai_ui/theme/colors.py:3` |
| `SURFACE` | `"#16161d"` | `mikai_ui/theme/colors.py:4` |
| `SURFACE_HOVER` | `"#22222b"` | `mikai_ui/theme/colors.py:5` |
| `SURFACE_ACTIVE` | `"#2b2b35"` | `mikai_ui/theme/colors.py:6` |
| `ACCENT` | `"#bdacff"` | `mikai_ui/theme/colors.py:7` |
| `ACCENT_HOVER` | `"#cfc4ff"` | `mikai_ui/theme/colors.py:8` |
| `DANGER` | `"#ff6b78"` | `mikai_ui/theme/colors.py:9` |
| `TEXT` | `"#e7e7ea"` | `mikai_ui/theme/colors.py:10` |
| `MUTED` | `"#a0a0ab"` | `mikai_ui/theme/colors.py:11` |
| `BORDER` | `"#30303a"` | `mikai_ui/theme/colors.py:12` |
| `VIDEO_BACKGROUND` | `"#000000"` | `mikai_ui/theme/colors.py:13` |
| `CHROME_FILL` | `"#121218"` | `mikai_ui/theme/colors.py:17` |
| `CHROME_HAIRLINE` | `"rgba(231, 231, 234, 0.09)"` | `mikai_ui/theme/colors.py:18` |
| `GHOST_HOVER` | `"rgba(231, 231, 234, 0.08)"` | `mikai_ui/theme/colors.py:22` |
| `GHOST_PRESSED` | `"rgba(231, 231, 234, 0.14)"` | `mikai_ui/theme/colors.py:23` |
| `ICON_DISABLED` | `"rgba(160, 160, 171, 0.45)"` | `mikai_ui/theme/colors.py:27` |

### Metrics

| token | value | defined at |
|---|---|---|
| `SPACING_XS` | `4` | `mikai_ui/theme/metrics.py:3` |
| `SPACING_SMALL` | `8` | `mikai_ui/theme/metrics.py:4` |
| `SPACING_MEDIUM` | `12` | `mikai_ui/theme/metrics.py:5` |
| `SPACING_LARGE` | `20` | `mikai_ui/theme/metrics.py:6` |
| `RADIUS_SMALL` | `8` | `mikai_ui/theme/metrics.py:8` |
| `RADIUS` | `12` | `mikai_ui/theme/metrics.py:9` |
| `RADIUS_LARGE` | `16` | `mikai_ui/theme/metrics.py:10` |
| `PRIMARY_CONTROL_SIZE` | `44` | `mikai_ui/theme/metrics.py:12` |
| `SECONDARY_CONTROL_SIZE` | `36` | `mikai_ui/theme/metrics.py:13` |
| `SETTINGS_CONTROL_SIZE` | `36` | `mikai_ui/theme/metrics.py:14` |
| `SEARCH_POSTER_WIDTH` | `56` | `mikai_ui/theme/metrics.py:15` |
| `SEARCH_POSTER_HEIGHT` | `78` | `mikai_ui/theme/metrics.py:16` |
| `SEARCH_ROW_MIN_HEIGHT` | `92` | `mikai_ui/theme/metrics.py:17` |
| `SEARCH_RESULTS_PER_PAGE` | `55` | `mikai_ui/theme/metrics.py:18` |
| `PAGER_CHIP_WINDOW` | `7` | `mikai_ui/theme/metrics.py:19` |
| `TITLE_COVER_WIDTH` | `188` | `mikai_ui/theme/metrics.py:20` |
| `TITLE_COVER_HEIGHT` | `268` | `mikai_ui/theme/metrics.py:21` |
| `TITLE_DESCRIPTION_PREVIEW_CHARS` | `210` | `mikai_ui/theme/metrics.py:22` |
| `COVER_VIEWER_MIN_WIDTH` | `640` | `mikai_ui/theme/metrics.py:23` |
| `COVER_VIEWER_MIN_HEIGHT` | `480` | `mikai_ui/theme/metrics.py:24` |
| `COVER_VIEWER_DEFAULT_WIDTH` | `1100` | `mikai_ui/theme/metrics.py:25` |
| `COVER_VIEWER_DEFAULT_HEIGHT` | `800` | `mikai_ui/theme/metrics.py:26` |
| `COVER_VIEWER_BAR_HEIGHT` | `52` | `mikai_ui/theme/metrics.py:27` |
| `COVER_ZOOM_MIN` | `0.1` | `mikai_ui/theme/metrics.py:28` |
| `COVER_ZOOM_MAX` | `8.0` | `mikai_ui/theme/metrics.py:29` |
| `COVER_ZOOM_STEP` | `1.25` | `mikai_ui/theme/metrics.py:30` |
| `EPISODE_CARD_MIN_WIDTH` | `320` | `mikai_ui/theme/metrics.py:31` |
| `EPISODE_CARD_MIN_HEIGHT` | `64` | `mikai_ui/theme/metrics.py:32` |
| `CAROUSEL_CARD_WIDTH` | `156` | `mikai_ui/theme/metrics.py:33` |
| `CAROUSEL_CARD_HEIGHT` | `248` | `mikai_ui/theme/metrics.py:34` |
| `CAROUSEL_POSTER_WIDTH` | `132` | `mikai_ui/theme/metrics.py:35` |
| `CAROUSEL_POSTER_HEIGHT` | `174` | `mikai_ui/theme/metrics.py:36` |
| `CAROUSEL_SCROLL_STEP` | `468` | `mikai_ui/theme/metrics.py:37` |
| `HOME_CARD_WIDTH` | `360` | `mikai_ui/theme/metrics.py:38` |
| `HOME_CARD_HEIGHT` | `140` | `mikai_ui/theme/metrics.py:39` |
| `HOME_POSTER_WIDTH` | `80` | `mikai_ui/theme/metrics.py:40` |
| `HOME_POSTER_HEIGHT` | `114` | `mikai_ui/theme/metrics.py:41` |
| `HOME_CAROUSEL_SCROLL_STEP` | `740` | `mikai_ui/theme/metrics.py:42` |
| `HOME_FEED_POSTER_WIDTH` | `34` | `mikai_ui/theme/metrics.py:43` |
| `HOME_FEED_POSTER_HEIGHT` | `48` | `mikai_ui/theme/metrics.py:44` |
| `ICON_SIZE` | `18` | `mikai_ui/theme/metrics.py:45` |
| `TIMELINE_GROOVE_HEIGHT` | `4` | `mikai_ui/theme/metrics.py:46` |
| `TIMELINE_HANDLE_SIZE` | `12` | `mikai_ui/theme/metrics.py:47` |
| `TIMELINE_ROW_HEIGHT` | `20` | `mikai_ui/theme/metrics.py:48` |
| `VOLUME_GROOVE_HEIGHT` | `3` | `mikai_ui/theme/metrics.py:49` |
| `VOLUME_HANDLE_SIZE` | `10` | `mikai_ui/theme/metrics.py:50` |
| `VOLUME_SLIDER_WIDTH` | `84` | `mikai_ui/theme/metrics.py:51` |
| `TIME_LABEL_MIN_WIDTH` | `96` | `mikai_ui/theme/metrics.py:52` |
| `PLAYER_FIELD_HEIGHT` | `32` | `mikai_ui/theme/metrics.py:53` |
| `PLAYER_FIELD_MIN_WIDTH` | `64` | `mikai_ui/theme/metrics.py:54` |
| `PLAYER_FIELD_ROW_HEIGHT` | `34` | `mikai_ui/theme/metrics.py:55` |
| `PLAYER_SKIP_WIDTH` | `56` | `mikai_ui/theme/metrics.py:56` |
| `PLAYER_PRIMARY_ICON_SIZE` | `24` | `mikai_ui/theme/metrics.py:57` |
| `PLAYER_DIVIDER_HEIGHT` | `20` | `mikai_ui/theme/metrics.py:58` |
| `SPINNER_WIDTH` | `80` | `mikai_ui/theme/metrics.py:59` |
| `SPINNER_HEIGHT` | `4` | `mikai_ui/theme/metrics.py:60` |
| `STATUS_CARD_MIN_WIDTH` | `320` | `mikai_ui/theme/metrics.py:61` |
| `TIMELINE_SCALE` | `10_000` | `mikai_ui/theme/metrics.py:62` |
| `PLAYER_BAR_MIN_HEIGHT` | `88` | `mikai_ui/theme/metrics.py:63` |
| `PLAYER_MIN_WIDTH` | `860` | `mikai_ui/theme/metrics.py:64` |
| `PLAYER_MIN_HEIGHT` | `520` | `mikai_ui/theme/metrics.py:65` |
| `PLAYER_DEFAULT_WIDTH` | `1120` | `mikai_ui/theme/metrics.py:66` |
| `PLAYER_DEFAULT_HEIGHT` | `700` | `mikai_ui/theme/metrics.py:67` |
| `QUALITY_CACHE_TTL_MS` | `1_800_000` | `mikai_ui/theme/metrics.py:68` |
| `QUALITY_CACHE_MAX_ENTRIES` | `240` | `mikai_ui/theme/metrics.py:69` |
| `QUALITY_CACHE_SAVE_DELAY_MS` | `1000` | `mikai_ui/theme/metrics.py:70` |
| `ANIMATION_DURATION_MS` | `180` | `mikai_ui/theme/metrics.py:71` |
| `AUTO_HIDE_MS` | `1500` | `mikai_ui/theme/metrics.py:72` |
| `TOAST_DURATION_MS` | `1400` | `mikai_ui/theme/metrics.py:73` |
| `SINGLE_CLICK_DELAY_MS` | `230` | `mikai_ui/theme/metrics.py:74` |

## SearchPage

Home discovery and search results. Carousels, feed rows and the range-chip pager.

Measured window: **1180x820**.

### Containers

| depth | class | objectName | size | hint | visible |
|---|---|---|---|---|---|
| 0 | `SearchPage` | — | 1180x820 | 876x454 | yes |
| 1 | `QLabel` | `heading` | 60x24 | 60x12 | yes |
| 1 | `RateLimitLabel` | `muted` | 120x24 | 120x12 | yes |
| 1 | `QPushButton` | `settingsChip` | 36x24 | 36x24 | yes |
| 1 | `QLineEdit` | — | 1038x22 | 210x22 | yes |
| 2 | `QToolButton` | — | 22x18 | 24x23 | no |
| 1 | `QPushButton` | `primary` | 80x20 | 80x20 | yes |
| 1 | `QLabel` | `sectionHeading` | 1132x12 | 144x12 | yes |
| 1 | `QLabel` | `status` | 1132x12 | 828x12 | yes |
| 1 | `QScrollArea` | `discoveryScroll` | 1132x654 | 388x288 | yes |
| 2 | `QWidget` | `qt_scrollarea_viewport` | 1118x654 | - | yes |
| 2 | `QWidget` | `qt_scrollarea_hcontainer` | 100x30 | 54x14 | no |
| 2 | `QWidget` | `qt_scrollarea_vcontainer` | 14x654 | 14x54 | yes |
| 1 | `QTabBar` | — | 640x480 | 0x0 | no |
| 2 | `QToolButton` | `ScrollLeftButton` | 100x30 | 24x23 | no |
| 2 | `QToolButton` | `ScrollRightButton` | 100x30 | 24x23 | no |
| 1 | `QListWidget` | — | 640x480 | 256x192 | no |
| 2 | `QWidget` | `qt_scrollarea_viewport` | 638x478 | - | no |
| 2 | `QWidget` | `qt_scrollarea_hcontainer` | 100x30 | 54x14 | no |
| 2 | `QWidget` | `qt_scrollarea_vcontainer` | 100x30 | 14x54 | no |
| 1 | `RangePager` | — | 640x480 | 452x31 | no |
| 2 | `QPushButton` | `pagerControl` | 640x480 | 80x20 | no |
| 2 | `QPushButton` | `pagerControl` | 640x480 | 80x20 | no |
| 2 | `QWidget` | — | 640x480 | 0x0 | no |
| 2 | `QPushButton` | `pagerControl` | 640x480 | 80x20 | no |
| 2 | `QPushButton` | `pagerControl` | 640x480 | 80x20 | no |
| 2 | `QLabel` | `muted` | 640x480 | 12x14 | no |
| 2 | `QSpinBox` | — | 640x480 | 48x23 | no |

### Styled elements

| objectName | size | position | text | class | defined at | QSS | shown |
|---|---|---|---|---|---|---|---|
| `heading` | 60x24 | 24,20 | Mikai | `QLabel` | `mikai_ui/main.py:123 (SearchPage)` | `theme/__init__.py:53` | yes |
| `muted` | 120x24 | 986,20 | RPM 42/300 | `RateLimitLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `settingsChip` | 36x24 | 1120,20 | — | `QPushButton` | `mikai_ui/main.py:131 (SearchPage)` | `theme/__init__.py:98` | yes |
| `primary` | 80x20 | 1076,59 | Пошук | `QPushButton` | `mikai_ui/main.py:146 (SearchPage)` | `theme/__init__.py:87` | yes |
| `sectionHeading` | 1132x12 | 24,94 | Рекомендації | `QLabel` | `mikai_ui/main.py:153 (SearchPage)` | `theme/__init__.py:54` | yes |
| `status` | 1132x12 | 24,120 | Натисніть картку, щоб відкрити тайтл · пошук за назвою доступний вище | `QLabel` | `mikai_ui/main.py:157 (SearchPage)` | — | yes |
| `discoveryScroll` | 1132x654 | 24,146 | — | `QScrollArea` | `mikai_ui/main.py:179 (SearchPage)` | `theme/__init__.py:116` | yes |
| `sectionHeading` | 216x20 | 0,0 | Нещодавно відкриті | `QLabel` | `mikai_ui/main.py:153 (SearchPage)` | `theme/__init__.py:54` | yes |
| `carouselControl` | 80x20 | 952,0 | ← | `QPushButton` | `mikai_ui/main.py:229 (SearchPage)` | `theme/__init__.py:105` | yes |
| `carouselControl` | 80x20 | 1038,0 | → | `QPushButton` | `mikai_ui/main.py:229 (SearchPage)` | `theme/__init__.py:105` | yes |
| `muted` | 1118x12 | 0,26 | 9 тайтлів | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `secondary` | not laid out | — | Повторити | `QPushButton` | `mikai_ui/main.py:238 (SearchPage)` | — | no |
| `animeCarousel` | 1118x152 | 0,44 | — | `QScrollArea` | `mikai_ui/main.py:246 (SearchPage)` | `theme/__init__.py:115` | yes |
| `animeCarouselTrack` | 3324x144 | 0,0 | — | `QWidget` | `mikai_ui/main.py:252 (SearchPage)` | `theme/__init__.py:117` | yes |
| `animeCard` | 360x140 | 2,2 | — | `TitleCard` | `mikai_ui/title_card.py:45 (TitleCard)` | `theme/__init__.py:101` | yes |
| `carouselPoster` | 80x114 | 12,12 | Постер недоступний | `QLabel` | `mikai_ui/title_page.py:665 (TitlePage)` | `theme/__init__.py:118` | yes |
| `carouselTitle` | 236x24 | 104,12 | Пожирач монстрів | `QLabel` | `mikai_ui/title_page.py:686 (TitlePage)` | `theme/__init__.py:121` | yes |
| `carouselStatus` | 236x12 | 104,40 | … | `QLabel` | _button factory_ | `theme/__init__.py:122` | yes |
| `carouselEpisodes` | 236x12 | 104,56 | … | `QLabel` | _button factory_ | `theme/__init__.py:124` | yes |
| `animeCard` | 360x140 | 372,2 | — | `TitleCard` | `mikai_ui/title_card.py:45 (TitleCard)` | `theme/__init__.py:101` | yes |
| `carouselPoster` | 80x114 | 12,12 | Постер недоступний | `QLabel` | `mikai_ui/title_page.py:665 (TitlePage)` | `theme/__init__.py:118` | yes |
| `carouselTitle` | 236x24 | 104,12 | Боруто: Нове покоління Наруто | `QLabel` | `mikai_ui/title_page.py:686 (TitlePage)` | `theme/__init__.py:121` | yes |
| `carouselStatus` | 236x12 | 104,40 | … | `QLabel` | _button factory_ | `theme/__init__.py:122` | yes |
| `carouselEpisodes` | 236x12 | 104,56 | … | `QLabel` | _button factory_ | `theme/__init__.py:124` | yes |
| `animeCard` | 360x140 | 742,2 | — | `TitleCard` | `mikai_ui/title_card.py:45 (TitleCard)` | `theme/__init__.py:101` | yes |
| `carouselPoster` | 80x114 | 12,12 | Постер недоступний | `QLabel` | `mikai_ui/title_page.py:665 (TitlePage)` | `theme/__init__.py:118` | yes |
| `carouselTitle` | 236x24 | 104,12 | Чорний смолоскип | `QLabel` | `mikai_ui/title_page.py:686 (TitlePage)` | `theme/__init__.py:121` | yes |
| `carouselStatus` | 236x12 | 104,40 | … | `QLabel` | _button factory_ | `theme/__init__.py:122` | yes |
| `carouselEpisodes` | 236x12 | 104,56 | … | `QLabel` | _button factory_ | `theme/__init__.py:124` | yes |
| `animeCard` | 360x140 | 1112,2 | — | `TitleCard` | `mikai_ui/title_card.py:45 (TitleCard)` | `theme/__init__.py:101` | yes |
| `carouselPoster` | 80x114 | 12,12 | Постер недоступний | `QLabel` | `mikai_ui/title_page.py:665 (TitlePage)` | `theme/__init__.py:118` | yes |
| `carouselTitle` | 236x24 | 104,12 | Бліч | `QLabel` | `mikai_ui/title_page.py:686 (TitlePage)` | `theme/__init__.py:121` | yes |
| `carouselStatus` | 236x12 | 104,40 | … | `QLabel` | _button factory_ | `theme/__init__.py:122` | yes |
| `carouselEpisodes` | 236x12 | 104,56 | … | `QLabel` | _button factory_ | `theme/__init__.py:124` | yes |
| `animeCard` | 360x140 | 1482,2 | — | `TitleCard` | `mikai_ui/title_card.py:45 (TitleCard)` | `theme/__init__.py:101` | yes |
| `carouselPoster` | 80x114 | 12,12 | Постер недоступний | `QLabel` | `mikai_ui/title_page.py:665 (TitlePage)` | `theme/__init__.py:118` | yes |
| `carouselTitle` | 236x24 | 104,12 | Детектив Конан | `QLabel` | `mikai_ui/title_page.py:686 (TitlePage)` | `theme/__init__.py:121` | yes |
| `carouselStatus` | 236x12 | 104,40 | … | `QLabel` | _button factory_ | `theme/__init__.py:122` | yes |
| `carouselEpisodes` | 236x12 | 104,56 | … | `QLabel` | _button factory_ | `theme/__init__.py:124` | yes |
| `animeCard` | 360x140 | 1852,2 | — | `TitleCard` | `mikai_ui/title_card.py:45 (TitleCard)` | `theme/__init__.py:101` | yes |
| `carouselPoster` | 80x114 | 12,12 | Постер недоступний | `QLabel` | `mikai_ui/title_page.py:665 (TitlePage)` | `theme/__init__.py:118` | yes |
| `carouselTitle` | 236x24 | 104,12 | Наруто | `QLabel` | `mikai_ui/title_page.py:686 (TitlePage)` | `theme/__init__.py:121` | yes |
| `carouselStatus` | 236x12 | 104,40 | … | `QLabel` | _button factory_ | `theme/__init__.py:122` | yes |
| `carouselEpisodes` | 236x12 | 104,56 | … | `QLabel` | _button factory_ | `theme/__init__.py:124` | yes |
| `animeCard` | 360x140 | 2222,2 | — | `TitleCard` | `mikai_ui/title_card.py:45 (TitleCard)` | `theme/__init__.py:101` | yes |
| `carouselPoster` | 80x114 | 12,12 | Постер недоступний | `QLabel` | `mikai_ui/title_page.py:665 (TitlePage)` | `theme/__init__.py:118` | yes |
| `carouselTitle` | 236x24 | 104,12 | Чорний Джек | `QLabel` | `mikai_ui/title_page.py:686 (TitlePage)` | `theme/__init__.py:121` | yes |
| `carouselStatus` | 236x12 | 104,40 | … | `QLabel` | _button factory_ | `theme/__init__.py:122` | yes |
| `carouselEpisodes` | 236x12 | 104,56 | … | `QLabel` | _button factory_ | `theme/__init__.py:124` | yes |
| `animeCard` | 360x140 | 2592,2 | — | `TitleCard` | `mikai_ui/title_card.py:45 (TitleCard)` | `theme/__init__.py:101` | yes |
| `carouselPoster` | 80x114 | 12,12 | Постер недоступний | `QLabel` | `mikai_ui/title_page.py:665 (TitlePage)` | `theme/__init__.py:118` | yes |
| `carouselTitle` | 236x24 | 104,12 | Бліч: Тисячолітня кривава війна | `QLabel` | `mikai_ui/title_page.py:686 (TitlePage)` | `theme/__init__.py:121` | yes |
| `carouselStatus` | 236x12 | 104,40 | … | `QLabel` | _button factory_ | `theme/__init__.py:122` | yes |
| `carouselEpisodes` | 236x12 | 104,56 | … | `QLabel` | _button factory_ | `theme/__init__.py:124` | yes |
| `animeCard` | 360x140 | 2962,2 | — | `TitleCard` | `mikai_ui/title_card.py:45 (TitleCard)` | `theme/__init__.py:101` | yes |
| `carouselPoster` | 80x114 | 12,12 | Постер недоступний | `QLabel` | `mikai_ui/title_page.py:665 (TitlePage)` | `theme/__init__.py:118` | yes |
| `carouselTitle` | 236x24 | 104,12 | Переродження: Життя з нуля в іншому світі (сезон 4) | `QLabel` | `mikai_ui/title_page.py:686 (TitlePage)` | `theme/__init__.py:121` | yes |
| `carouselStatus` | 236x12 | 104,40 | … | `QLabel` | _button factory_ | `theme/__init__.py:122` | yes |
| `carouselEpisodes` | 236x12 | 104,56 | … | `QLabel` | _button factory_ | `theme/__init__.py:124` | yes |
| `sectionHeading` | 168x20 | 0,0 | Зараз виходять | `QLabel` | `mikai_ui/main.py:153 (SearchPage)` | `theme/__init__.py:54` | yes |
| `carouselControl` | 80x20 | 952,0 | ← | `QPushButton` | `mikai_ui/main.py:229 (SearchPage)` | `theme/__init__.py:105` | yes |
| `carouselControl` | 80x20 | 1038,0 | → | `QPushButton` | `mikai_ui/main.py:229 (SearchPage)` | `theme/__init__.py:105` | yes |
| `muted` | 1118x12 | 0,26 | 3 тайтлів | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `secondary` | not laid out | — | Повторити | `QPushButton` | `mikai_ui/main.py:238 (SearchPage)` | — | no |
| `animeCarousel` | 1118x152 | 0,44 | — | `QScrollArea` | `mikai_ui/main.py:246 (SearchPage)` | `theme/__init__.py:115` | yes |
| `animeCarouselTrack` | 1104x144 | 0,0 | — | `QWidget` | `mikai_ui/main.py:252 (SearchPage)` | `theme/__init__.py:117` | yes |
| `animeCard` | 360x140 | 2,2 | — | `TitleCard` | `mikai_ui/title_card.py:45 (TitleCard)` | `theme/__init__.py:101` | yes |
| `carouselPoster` | 80x114 | 12,12 | — | `QLabel` | `mikai_ui/title_page.py:665 (TitlePage)` | `theme/__init__.py:118` | yes |
| `carouselTitle` | 236x24 | 104,12 | Чорний смолоскип | `QLabel` | `mikai_ui/title_page.py:686 (TitlePage)` | `theme/__init__.py:121` | yes |
| `carouselStatus` | 236x12 | 104,40 | … | `QLabel` | _button factory_ | `theme/__init__.py:122` | yes |
| `carouselEpisodes` | 236x12 | 104,56 | … | `QLabel` | _button factory_ | `theme/__init__.py:124` | yes |
| `animeCard` | 360x140 | 372,2 | — | `TitleCard` | `mikai_ui/title_card.py:45 (TitleCard)` | `theme/__init__.py:101` | yes |
| `carouselPoster` | 80x114 | 12,12 | — | `QLabel` | `mikai_ui/title_page.py:665 (TitlePage)` | `theme/__init__.py:118` | yes |
| `carouselTitle` | 236x24 | 104,12 | Чорний Джек | `QLabel` | `mikai_ui/title_page.py:686 (TitlePage)` | `theme/__init__.py:121` | yes |
| `carouselStatus` | 236x12 | 104,40 | … | `QLabel` | _button factory_ | `theme/__init__.py:122` | yes |
| `carouselEpisodes` | 236x12 | 104,56 | … | `QLabel` | _button factory_ | `theme/__init__.py:124` | yes |
| `animeCard` | 360x140 | 742,2 | — | `TitleCard` | `mikai_ui/title_card.py:45 (TitleCard)` | `theme/__init__.py:101` | yes |
| `carouselPoster` | 80x114 | 12,12 | Постер недоступний | `QLabel` | `mikai_ui/title_page.py:665 (TitlePage)` | `theme/__init__.py:118` | yes |
| `carouselTitle` | 236x24 | 104,12 | Без постера | `QLabel` | `mikai_ui/title_page.py:686 (TitlePage)` | `theme/__init__.py:121` | yes |
| `carouselStatus` | 236x12 | 104,40 | … | `QLabel` | _button factory_ | `theme/__init__.py:122` | yes |
| `carouselEpisodes` | 236x12 | 104,56 | … | `QLabel` | _button factory_ | `theme/__init__.py:124` | yes |
| `sectionHeading` | 216x20 | 0,0 | Популярні на Mikai | `QLabel` | `mikai_ui/main.py:153 (SearchPage)` | `theme/__init__.py:54` | yes |
| `carouselControl` | 80x20 | 952,0 | ← | `QPushButton` | `mikai_ui/main.py:229 (SearchPage)` | `theme/__init__.py:105` | yes |
| `carouselControl` | 80x20 | 1038,0 | → | `QPushButton` | `mikai_ui/main.py:229 (SearchPage)` | `theme/__init__.py:105` | yes |
| `muted` | 1118x12 | 0,26 | 3 тайтлів | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `secondary` | not laid out | — | Повторити | `QPushButton` | `mikai_ui/main.py:238 (SearchPage)` | — | no |
| `animeCarousel` | 1118x152 | 0,44 | — | `QScrollArea` | `mikai_ui/main.py:246 (SearchPage)` | `theme/__init__.py:115` | yes |
| `animeCarouselTrack` | 1104x144 | 0,0 | — | `QWidget` | `mikai_ui/main.py:252 (SearchPage)` | `theme/__init__.py:117` | yes |
| `animeCard` | 360x140 | 2,2 | — | `TitleCard` | `mikai_ui/title_card.py:45 (TitleCard)` | `theme/__init__.py:101` | yes |
| `carouselPoster` | 80x114 | 12,12 | — | `QLabel` | `mikai_ui/title_page.py:665 (TitlePage)` | `theme/__init__.py:118` | yes |
| `carouselTitle` | 236x24 | 104,12 | Чорний смолоскип | `QLabel` | `mikai_ui/title_page.py:686 (TitlePage)` | `theme/__init__.py:121` | yes |
| `carouselStatus` | 236x12 | 104,40 | … | `QLabel` | _button factory_ | `theme/__init__.py:122` | yes |
| `carouselEpisodes` | 236x12 | 104,56 | … | `QLabel` | _button factory_ | `theme/__init__.py:124` | yes |
| `animeCard` | 360x140 | 372,2 | — | `TitleCard` | `mikai_ui/title_card.py:45 (TitleCard)` | `theme/__init__.py:101` | yes |
| `carouselPoster` | 80x114 | 12,12 | — | `QLabel` | `mikai_ui/title_page.py:665 (TitlePage)` | `theme/__init__.py:118` | yes |
| `carouselTitle` | 236x24 | 104,12 | Чорний Джек | `QLabel` | `mikai_ui/title_page.py:686 (TitlePage)` | `theme/__init__.py:121` | yes |
| `carouselStatus` | 236x12 | 104,40 | … | `QLabel` | _button factory_ | `theme/__init__.py:122` | yes |
| `carouselEpisodes` | 236x12 | 104,56 | … | `QLabel` | _button factory_ | `theme/__init__.py:124` | yes |
| `animeCard` | 360x140 | 742,2 | — | `TitleCard` | `mikai_ui/title_card.py:45 (TitleCard)` | `theme/__init__.py:101` | yes |
| `carouselPoster` | 80x114 | 12,12 | Постер недоступний | `QLabel` | `mikai_ui/title_page.py:665 (TitlePage)` | `theme/__init__.py:118` | yes |
| `carouselTitle` | 236x24 | 104,12 | Без постера | `QLabel` | `mikai_ui/title_page.py:686 (TitlePage)` | `theme/__init__.py:121` | yes |
| `carouselStatus` | 236x12 | 104,40 | … | `QLabel` | _button factory_ | `theme/__init__.py:122` | yes |
| `carouselEpisodes` | 236x12 | 104,56 | … | `QLabel` | _button factory_ | `theme/__init__.py:124` | yes |
| `sectionHeading` | 204x20 | 0,0 | Останні оновлення | `QLabel` | `mikai_ui/main.py:153 (SearchPage)` | `theme/__init__.py:54` | yes |
| `textAction` | 98x20 | 1020,0 | Оновити | `QPushButton` | `mikai_ui/main.py:293 (SearchPage)` | `theme/__init__.py:90` | yes |
| `muted` | 1118x12 | 0,26 | Нових оновлень поки немає. | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `sectionHeading` | 168x20 | 0,0 | Розклад виходу | `QLabel` | `mikai_ui/main.py:153 (SearchPage)` | `theme/__init__.py:54` | yes |
| `textAction` | 98x20 | 1020,0 | Оновити | `QPushButton` | `mikai_ui/main.py:293 (SearchPage)` | `theme/__init__.py:90` | yes |
| `muted` | 1118x12 | 0,26 | У розкладі поки немає виходів. | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `ScrollLeftButton` | not laid out | — | — | `QToolButton` | _button factory_ | — | no |
| `ScrollRightButton` | not laid out | — | — | `QToolButton` | _button factory_ | — | no |
| `pagerControl` | not laid out | — | ⏮ | `QPushButton` | `mikai_ui/page_pager.py:74 (RangePager)` | `theme/__init__.py:166` | no |
| `pagerControl` | not laid out | — | ← | `QPushButton` | `mikai_ui/page_pager.py:74 (RangePager)` | `theme/__init__.py:166` | no |
| `pagerControl` | not laid out | — | → | `QPushButton` | `mikai_ui/page_pager.py:74 (RangePager)` | `theme/__init__.py:166` | no |
| `pagerControl` | not laid out | — | ⏭ | `QPushButton` | `mikai_ui/page_pager.py:74 (RangePager)` | `theme/__init__.py:166` | no |
| `muted` | not laid out | — | — | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |

## TitlePage

One title: detail header, release tabs, per-episode cards and quality chips.

Measured window: **1180x820**.

### Containers

| depth | class | objectName | size | hint | visible |
|---|---|---|---|---|---|
| 0 | `TitlePage` | — | 1180x820 | 828x757 | yes |
| 1 | `QPushButton` | — | 98x20 | 98x20 | yes |
| 1 | `QLabel` | `status` | 0x24 | 0x12 | yes |
| 1 | `RateLimitLabel` | `muted` | 120x24 | 120x12 | yes |
| 1 | `QPushButton` | `settingsChip` | 36x24 | 36x24 | yes |
| 1 | `QScrollArea` | `titleOverview` | 1132x333 | 432x270 | yes |
| 2 | `QWidget` | `qt_scrollarea_viewport` | 1116x331 | - | yes |
| 2 | `QWidget` | `qt_scrollarea_hcontainer` | 100x30 | 54x14 | no |
| 2 | `QWidget` | `qt_scrollarea_vcontainer` | 14x331 | 14x54 | yes |
| 1 | `QLabel` | `sectionHeading` | 180x22 | 180x12 | yes |
| 1 | `QComboBox` | — | 766x22 | 114x22 | yes |
| 2 | `QFrame` | — | 100x30 | 2x2 | no |
| 1 | `QPushButton` | — | 170x20 | 170x20 | yes |
| 1 | `QTabWidget` | — | 1132x359 | 780x353 | yes |
| 2 | `QStackedWidget` | `qt_tabwidget_stackedwidget` | 1128x333 | 776x325 | yes |
| 2 | `QTabBar` | `qt_tabwidget_tabbar` | 324x24 | 324x24 | yes |
| 1 | `QLineEdit` | — | 640x480 | 210x22 | no |

### Styled elements

| objectName | size | position | text | class | defined at | QSS | shown |
|---|---|---|---|---|---|---|---|
| `status` | 0x24 | 972,20 | — | `QLabel` | `mikai_ui/main.py:157 (SearchPage)` | — | yes |
| `muted` | 120x24 | 986,20 | RPM 42/300 | `RateLimitLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `settingsChip` | 36x24 | 1120,20 | — | `QPushButton` | `mikai_ui/main.py:131 (SearchPage)` | `theme/__init__.py:98` | yes |
| `titleOverview` | 1132x333 | 24,58 | — | `QScrollArea` | `mikai_ui/title_page.py:266 (TitlePage)` | — | yes |
| `titleCover` | 188x268 | 0,0 | — | `CoverLabel` | `mikai_ui/title_page.py:276 (TitlePage)` | `theme/__init__.py:59` | yes |
| `heading` | 904x12 | 212,0 | Чорний смолоскип | `QLabel` | `mikai_ui/main.py:123 (SearchPage)` | `theme/__init__.py:53` | yes |
| `muted` | 904x12 | 212,20 | BLACK TORCH | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `titleFacts` | 904x12 | 212,40 | ТБ серіал   ·   Літо 2026 | `QLabel` | `mikai_ui/title_page.py:297 (TitlePage)` | `theme/__init__.py:61` | yes |
| `titleDescription` | 904x42 | 212,82 | Тестовий опис. Тестовий опис. Тестовий опис. Тестовий опис. Тестовий опис. Тестовий опис. Тестовий опис. Тестовий опис. Тестовий опис. Тестовий опис. Тестовий опис. Тестовий опис. Тестовий опис. Тестовий опис… | `QLabel` | `mikai_ui/title_page.py:306 (TitlePage)` | `theme/__init__.py:62` | yes |
| `textAction` | 146x20 | 212,132 | Читати далі | `QPushButton` | `mikai_ui/main.py:293 (SearchPage)` | `theme/__init__.py:90` | yes |
| `textAction` | 254x20 | 366,132 | Докладніше про тайтл | `QPushButton` | `mikai_ui/main.py:293 (SearchPage)` | `theme/__init__.py:90` | yes |
| `textAction` | 170x20 | 628,132 | Сховати схожі | `QPushButton` | `mikai_ui/main.py:293 (SearchPage)` | `theme/__init__.py:90` | yes |
| `titleDetails` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:336 (TitlePage)` | `theme/__init__.py:63` | no |
| `muted` | not laid out | — | Тип | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `muted` | not laid out | — | Сезон | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `sectionHeading` | 252x20 | 0,0 | Рекомендації на Mikai | `QLabel` | `mikai_ui/main.py:153 (SearchPage)` | `theme/__init__.py:54` | yes |
| `carouselControl` | 80x20 | 738,0 | ← | `QPushButton` | `mikai_ui/main.py:229 (SearchPage)` | `theme/__init__.py:105` | yes |
| `carouselControl` | 80x20 | 824,0 | → | `QPushButton` | `mikai_ui/main.py:229 (SearchPage)` | `theme/__init__.py:105` | yes |
| `animeCarousel` | 904x260 | 0,26 | — | `QScrollArea` | `mikai_ui/main.py:246 (SearchPage)` | `theme/__init__.py:115` | yes |
| `animeCarouselTrack` | 326x252 | 0,0 | — | `QWidget` | `mikai_ui/main.py:252 (SearchPage)` | `theme/__init__.py:117` | yes |
| `animeCard` | 156x248 | 2,2 | — | `QPushButton` | `mikai_ui/title_card.py:45 (TitleCard)` | `theme/__init__.py:101` | yes |
| `carouselPoster` | 132x174 | 12,8 | Постер недоступний | `QLabel` | `mikai_ui/title_page.py:665 (TitlePage)` | `theme/__init__.py:118` | yes |
| `carouselKind` | 140x12 | 8,184 | Recommendation | `QLabel` | `mikai_ui/title_page.py:680 (TitlePage)` | `theme/__init__.py:120` | yes |
| `carouselTitle` | 140x38 | 8,202 | Чорний Джек | `QLabel` | `mikai_ui/title_page.py:686 (TitlePage)` | `theme/__init__.py:121` | yes |
| `animeCard` | 156x248 | 168,2 | — | `QPushButton` | `mikai_ui/title_card.py:45 (TitleCard)` | `theme/__init__.py:101` | yes |
| `carouselPoster` | 132x174 | 12,8 | Постер недоступний | `QLabel` | `mikai_ui/title_page.py:665 (TitlePage)` | `theme/__init__.py:118` | yes |
| `carouselKind` | 140x12 | 8,184 | Recommendation | `QLabel` | `mikai_ui/title_page.py:680 (TitlePage)` | `theme/__init__.py:120` | yes |
| `carouselTitle` | 140x38 | 8,202 | Без постера | `QLabel` | `mikai_ui/title_page.py:686 (TitlePage)` | `theme/__init__.py:121` | yes |
| `sectionHeading` | 180x22 | 24,405 | Серії та якість | `QLabel` | `mikai_ui/main.py:153 (SearchPage)` | `theme/__init__.py:54` | yes |
| `episodeScroll` | not laid out | — | — | `QScrollArea` | `mikai_ui/title_page.py:797 (TitlePage)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 1 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 2 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 3 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 4 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 5 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 6 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 7 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 8 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 9 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 10 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 11 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 12 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `pagerControl` | not laid out | — | ⏮ | `QPushButton` | `mikai_ui/page_pager.py:74 (RangePager)` | `theme/__init__.py:166` | no |
| `pagerControl` | not laid out | — | ← | `QPushButton` | `mikai_ui/page_pager.py:74 (RangePager)` | `theme/__init__.py:166` | no |
| `pagerChipActive` | not laid out | — | 1 – 12 | `QPushButton` | _button factory_ | `theme/__init__.py:174` | no |
| `pagerChip` | not laid out | — | 13 – 24 | `QPushButton` | _button factory_ | `theme/__init__.py:171` | no |
| `pagerControl` | not laid out | — | → | `QPushButton` | `mikai_ui/page_pager.py:74 (RangePager)` | `theme/__init__.py:166` | no |
| `pagerControl` | not laid out | — | ⏭ | `QPushButton` | `mikai_ui/page_pager.py:74 (RangePager)` | `theme/__init__.py:166` | no |
| `muted` | not laid out | — | сторінка з 2 | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeScroll` | not laid out | — | — | `QScrollArea` | `mikai_ui/title_page.py:797 (TitlePage)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 1 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 2 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 3 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 4 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 5 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 6 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 7 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 8 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 9 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 10 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 11 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeCard` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | no |
| `episodeCopy` | not laid out | — | Серія 12 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | no |
| `qualityHolder` | not laid out | — | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | no |
| `muted` | not laid out | — | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `pagerControl` | not laid out | — | ⏮ | `QPushButton` | `mikai_ui/page_pager.py:74 (RangePager)` | `theme/__init__.py:166` | no |
| `pagerControl` | not laid out | — | ← | `QPushButton` | `mikai_ui/page_pager.py:74 (RangePager)` | `theme/__init__.py:166` | no |
| `pagerChipActive` | not laid out | — | 1 – 12 | `QPushButton` | _button factory_ | `theme/__init__.py:174` | no |
| `pagerChip` | not laid out | — | 13 – 24 | `QPushButton` | _button factory_ | `theme/__init__.py:171` | no |
| `pagerControl` | not laid out | — | → | `QPushButton` | `mikai_ui/page_pager.py:74 (RangePager)` | `theme/__init__.py:166` | no |
| `pagerControl` | not laid out | — | ⏭ | `QPushButton` | `mikai_ui/page_pager.py:74 (RangePager)` | `theme/__init__.py:166` | no |
| `muted` | not laid out | — | сторінка з 2 | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `episodeScroll` | 1128x296 | 0,0 | — | `QScrollArea` | `mikai_ui/title_page.py:797 (TitlePage)` | — | yes |
| `episodeCard` | 360x64 | 8,8 | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | yes |
| `episodeCopy` | 98x20 | 12,22 | Серія 1 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | yes |
| `qualityHolder` | 84x48 | 264,8 | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | yes |
| `muted` | 84x48 | 0,0 | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `episodeCard` | 360x64 | 376,8 | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | yes |
| `episodeCopy` | 98x20 | 12,22 | Серія 2 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | yes |
| `qualityHolder` | 84x48 | 264,8 | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | yes |
| `muted` | 84x48 | 0,0 | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `episodeCard` | 360x64 | 744,8 | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | yes |
| `episodeCopy` | 98x20 | 12,22 | Серія 3 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | yes |
| `qualityHolder` | 84x48 | 264,8 | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | yes |
| `muted` | 84x48 | 0,0 | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `episodeCard` | 360x64 | 8,80 | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | yes |
| `episodeCopy` | 98x20 | 12,22 | Серія 4 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | yes |
| `qualityHolder` | 84x48 | 264,8 | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | yes |
| `muted` | 84x48 | 0,0 | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `episodeCard` | 360x64 | 376,80 | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | yes |
| `episodeCopy` | 98x20 | 12,22 | Серія 5 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | yes |
| `qualityHolder` | 84x48 | 264,8 | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | yes |
| `muted` | 84x48 | 0,0 | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `episodeCard` | 360x64 | 744,80 | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | yes |
| `episodeCopy` | 98x20 | 12,22 | Серія 6 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | yes |
| `qualityHolder` | 84x48 | 264,8 | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | yes |
| `muted` | 84x48 | 0,0 | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `episodeCard` | 360x64 | 8,152 | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | yes |
| `episodeCopy` | 98x20 | 12,22 | Серія 7 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | yes |
| `qualityHolder` | 84x48 | 264,8 | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | yes |
| `muted` | 84x48 | 0,0 | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `episodeCard` | 360x64 | 376,152 | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | yes |
| `episodeCopy` | 98x20 | 12,22 | Серія 8 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | yes |
| `qualityHolder` | 84x48 | 264,8 | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | yes |
| `muted` | 84x48 | 0,0 | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `episodeCard` | 360x64 | 744,152 | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | yes |
| `episodeCopy` | 98x20 | 12,22 | Серія 9 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | yes |
| `qualityHolder` | 84x48 | 264,8 | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | yes |
| `muted` | 84x48 | 0,0 | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `episodeCard` | 360x64 | 8,224 | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | yes |
| `episodeCopy` | 110x20 | 12,22 | Серія 10 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | yes |
| `qualityHolder` | 84x48 | 264,8 | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | yes |
| `muted` | 84x48 | 0,0 | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `episodeCard` | 360x64 | 376,224 | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | yes |
| `episodeCopy` | 110x20 | 12,22 | Серія 11 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | yes |
| `qualityHolder` | 84x48 | 264,8 | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | yes |
| `muted` | 84x48 | 0,0 | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `episodeCard` | 360x64 | 744,224 | — | `QWidget` | `mikai_ui/title_page.py:910 (TitlePage)` | `theme/__init__.py:66` | yes |
| `episodeCopy` | 110x20 | 12,22 | Серія 12 | `QPushButton` | `mikai_ui/title_page.py:935 (TitlePage)` | `theme/__init__.py:93` | yes |
| `qualityHolder` | 84x48 | 264,8 | — | `QWidget` | `mikai_ui/title_page.py:926 (TitlePage)` | `theme/__init__.py:69` | yes |
| `muted` | 84x48 | 0,0 | Якість… | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `pagerControl` | 80x20 | 8,5 | ⏮ | `QPushButton` | `mikai_ui/page_pager.py:74 (RangePager)` | `theme/__init__.py:166` | yes |
| `pagerControl` | 80x20 | 96,5 | ← | `QPushButton` | `mikai_ui/page_pager.py:74 (RangePager)` | `theme/__init__.py:166` | yes |
| `pagerChipActive` | 86x20 | 0,1 | 1 – 12 | `QPushButton` | _button factory_ | `theme/__init__.py:174` | yes |
| `pagerChip` | 98x20 | 94,1 | 13 – 24 | `QPushButton` | _button factory_ | `theme/__init__.py:171` | yes |
| `pagerControl` | 80x20 | 736,5 | → | `QPushButton` | `mikai_ui/page_pager.py:74 (RangePager)` | `theme/__init__.py:166` | yes |
| `pagerControl` | 80x20 | 824,5 | ⏭ | `QPushButton` | `mikai_ui/page_pager.py:74 (RangePager)` | `theme/__init__.py:166` | yes |
| `muted` | 144x23 | 920,4 | сторінка з 2 | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `ScrollLeftButton` | not laid out | — | — | `QToolButton` | _button factory_ | — | no |
| `ScrollRightButton` | not laid out | — | — | `QToolButton` | _button factory_ | — | no |

## PlayerDialog

Embedded mpv player. The bar is a sibling of the video, never an overlay.

Measured window: **800x800**.

### Containers

| depth | class | objectName | size | hint | visible |
|---|---|---|---|---|---|
| 0 | `PlayerDialog` | `playerWindow` | 800x800 | 741x88 | yes |
| 1 | `QWidget` | `playerHeader` | 1120x36 | 232x36 | no |
| 2 | `QLabel` | `playerTitle` | 1080x12 | 192x12 | no |
| 1 | `QWidget` | `videoStage` | 800x800 | 741x88 | yes |
| 2 | `QWidget` | `videoSurface` | 800x712 | - | yes |
| 2 | `PlayerControlsWidget` | `playerControls` | 800x88 | 741x88 | yes |
| 2 | `PlayerOverlayWidget` | `playerOverlay` | 100x30 | - | no |
| 2 | `QLabel` | `playerToast` | 100x30 | 12x14 | no |
| 2 | `QWidget` | `playerStatusCard` | 320x86 | 120x78 | no |
| 2 | `QLabel` | `playerStreamChip` | 192x12 | 192x12 | yes |

### Styled elements

| objectName | size | position | text | class | defined at | QSS | shown |
|---|---|---|---|---|---|---|---|
| `playerWindow` | 800x800 | 0,0 | — | `PlayerDialog` | `mikai_ui/player_dialog.py:61 (PlayerDialog)` | `theme/__init__.py:231` | yes |
| `playerHeader` | not laid out | — | — | `QWidget` | `mikai_ui/player_dialog.py:73 (PlayerDialog)` | `theme/__init__.py:183` | no |
| `playerTitle` | not laid out | — | Серія 12 · Ashdi | `QLabel` | `mikai_ui/player_dialog.py:79 (PlayerDialog)` | `theme/__init__.py:234` | no |
| `videoStage` | 800x800 | 0,0 | — | `QWidget` | `mikai_ui/player_dialog.py:86 (PlayerDialog)` | — | yes |
| `videoSurface` | 800x712 | 0,0 | — | `QWidget` | `mikai_ui/player_dialog.py:98 (PlayerDialog)` | — | yes |
| `playerControls` | 800x88 | 0,712 | — | `PlayerControlsWidget` | `mikai_ui/player_controls.py:166 (PlayerControlsWidget)` | `theme/__init__.py:177` | yes |
| `timeline` | 776x20 | 12,8 | — | `TimelineSlider` | `mikai_ui/player_controls.py:68 (TimelineSlider)` | `theme/__init__.py:190` | yes |
| `playerSkip` | 56x36 | 12,40 | 10 | `QPushButton` | `mikai_ui/player_controls.py:255 (PlayerControlsWidget)` | `theme/__init__.py:138` | yes |
| `playerPrimary` | 44x44 | 76,36 | — | `QPushButton` | _button factory_ | `theme/__init__.py:125` | yes |
| `playerSkip` | 56x36 | 128,40 | 10 | `QPushButton` | `mikai_ui/player_controls.py:255 (PlayerControlsWidget)` | `theme/__init__.py:138` | yes |
| `playerTime` | 144x44 | 200,36 | 7:01 / 24:00 | `QLabel` | `mikai_ui/player_controls.py:178 (PlayerControlsWidget)` | `theme/__init__.py:165` | yes |
| `playerDivider` | 1x20 | 411,48 | — | `QWidget` | `mikai_ui/player_controls.py:268 (PlayerControlsWidget)` | `theme/__init__.py:146` | yes |
| `playerSecondary` | 36x36 | 420,40 | — | `QPushButton` | _button factory_ | `theme/__init__.py:132` | yes |
| `volume` | 84x15 | 464,50 | — | `QSlider` | `mikai_ui/player_controls.py:215 (PlayerControlsWidget)` | `theme/__init__.py:202` | yes |
| `playerField` | 90x22 | 556,47 | — | `QComboBox` | `mikai_ui/player_controls.py:225 (PlayerControlsWidget)` | `theme/__init__.py:151` | yes |
| `playerField` | 90x22 | 654,47 | — | `QComboBox` | `mikai_ui/player_controls.py:225 (PlayerControlsWidget)` | `theme/__init__.py:151` | yes |
| `playerSecondary` | 36x36 | 752,40 | — | `QPushButton` | _button factory_ | `theme/__init__.py:132` | yes |
| `playerOverlay` | not laid out | — | — | `PlayerOverlayWidget` | `mikai_ui/player_overlay.py:56 (PlayerOverlayWidget)` | `theme/__init__.py:236` | no |
| `playerToast` | not laid out | — | — | `QLabel` | `mikai_ui/player_overlay.py:123 (PlayerOverlayWidget)` | `theme/__init__.py:184` | no |
| `playerStatusCard` | not laid out | — | — | `QWidget` | `mikai_ui/player_overlay.py:69 (PlayerOverlayWidget)` | `theme/__init__.py:179` | no |
| `playerStatusTitle` | not laid out | — | — | `QLabel` | `mikai_ui/player_overlay.py:76 (PlayerOverlayWidget)` | `theme/__init__.py:181` | no |
| `muted` | not laid out | — | — | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | no |
| `playerSpinner` | not laid out | — | — | `QProgressBar` | `mikai_ui/player_overlay.py:85 (PlayerOverlayWidget)` | `theme/__init__.py:210` | no |
| `primary` | not laid out | — | Повторити | `QPushButton` | `mikai_ui/main.py:146 (SearchPage)` | `theme/__init__.py:87` | no |
| `playerGhost` | not laid out | — | Деталі | `QPushButton` | `mikai_ui/player_overlay.py:101 (PlayerOverlayWidget)` | `theme/__init__.py:148` | no |
| `playerStreamChip` | 192x12 | 20,20 | 1280x720 | 24fps | `QLabel` | `mikai_ui/player_overlay.py:116 (PlayerOverlayWidget)` | `theme/__init__.py:187` | yes |

## SettingsDialog

API key, live quota and the home-page block toggles.

### Containers

| depth | class | objectName | size | hint | visible |
|---|---|---|---|---|---|
| 0 | `SettingsDialog` | — | 533x526 | 636x526 | yes |
| 1 | `QLabel` | — | 22x22 | 22x22 | yes |
| 1 | `QLabel` | `heading` | 276x22 | 276x12 | yes |
| 1 | `QLabel` | `muted` | 485x40 | 468x40 | yes |
| 1 | `QLabel` | — | 485x14 | 588x14 | yes |
| 1 | `QLineEdit` | — | 485x22 | 236x22 | yes |
| 2 | `QToolButton` | — | 22x18 | 24x23 | yes |
| 1 | `QLabel` | `sectionHeading` | 485x12 | 192x12 | yes |
| 1 | `QLabel` | `descriptionPanel` | 485x40 | 480x68 | yes |
| 1 | `QLabel` | `sectionHeading` | 485x12 | 276x12 | yes |
| 1 | `QLabel` | `muted` | 485x40 | 444x40 | yes |
| 1 | `QCheckBox` | — | 485x17 | 192x17 | yes |
| 1 | `QCheckBox` | — | 485x17 | 240x17 | yes |
| 1 | `QCheckBox` | — | 485x17 | 228x17 | yes |
| 1 | `QCheckBox` | — | 485x17 | 192x17 | yes |
| 1 | `QLabel` | `status` | 485x14 | 12x14 | yes |
| 1 | `QPushButton` | — | 134x20 | 134x20 | yes |
| 1 | `QPushButton` | — | 122x20 | 122x20 | yes |
| 1 | `QPushButton` | `primary` | 110x20 | 110x20 | yes |

### Styled elements

| objectName | size | position | text | class | defined at | QSS | shown |
|---|---|---|---|---|---|---|---|
| `heading` | 276x22 | 60,20 | Токен доступу Mikai API | `QLabel` | `mikai_ui/main.py:123 (SearchPage)` | `theme/__init__.py:53` | yes |
| `muted` | 485x40 | 24,56 | Ключ доступу підіймає ліміт з 60 до 300 запитів на хвилину і зберігається локально в .env файлі. | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `sectionHeading` | 485x12 | 24,174 | Використання API | `QLabel` | `mikai_ui/main.py:153 (SearchPage)` | `theme/__init__.py:54` | yes |
| `descriptionPanel` | 485x40 | 24,200 | За вікно: використано 42, доступно 258 із 300 запитів · оновлення через 17 с
Активні: 1 · у черзі: 2 · запити продовжаться автоматично після оновлення ліміту | `QLabel` | `mikai_ui/settings_dialog.py:83 (SettingsDialog)` | `theme/__init__.py:56` | yes |
| `sectionHeading` | 485x12 | 24,254 | Блоки головної сторінки | `QLabel` | `mikai_ui/main.py:153 (SearchPage)` | `theme/__init__.py:54` | yes |
| `muted` | 485x40 | 24,280 | Вимкнений блок не завантажується і не витрачає запити API. Пошук і сторінка тайтлу працюють завжди. | `QLabel` | `mikai_ui/cover_viewer.py:216 (CoverViewer)` | — | yes |
| `status` | 485x14 | 24,458 | — | `QLabel` | `mikai_ui/main.py:157 (SearchPage)` | — | yes |
| `primary` | 110x20 | 399,486 | Зберегти | `QPushButton` | `mikai_ui/main.py:146 (SearchPage)` | `theme/__init__.py:87` | yes |

## Modules

| module | classes |
|---|---|
| `mikai_ui/cover_viewer.py` | `CoverLabel`, `CoverView`, `CoverViewer` |
| `mikai_ui/fullscreen_manager.py` | `FullscreenManager` |
| `mikai_ui/main.py` | `MainWindow`, `SearchPage` |
| `mikai_ui/mpv_backend.py` | `MPVAdapter` |
| `mikai_ui/page_pager.py` | `RangePager` |
| `mikai_ui/player_controller.py` | `PlaybackState`, `PlaybackStatus`, `PlayerController`, `StreamInfo`, `_ResumePoint` |
| `mikai_ui/player_controls.py` | `FieldRowDelegate`, `PlayerControlsWidget`, `TimelineSlider` |
| `mikai_ui/player_dialog.py` | `PlayerDialog` |
| `mikai_ui/player_overlay.py` | `PlayerOverlayWidget` |
| `mikai_ui/rate_limit_label.py` | `RateLimitLabel` |
| `mikai_ui/settings_dialog.py` | `SettingsDialog` |
| `mikai_ui/title_card.py` | `TitleCard` |
| `mikai_ui/title_page.py` | `EpisodeGrid`, `TitlePage` |
| `mikai_ui/workers.py` | `Runner`, `Task`, `TaskSignals` |

## Measurement caveats

- Sizes come from a live Qt run on the `offscreen` platform, so they reflect
  layout arithmetic, not a real compositor. A window manager may clamp or
  decorate differently; trust the tokens for intent and these numbers for shape.
- `objectName` is not unique across the app. Where a name is reused (for
  example `muted` and `primary`), the table shows the first definition site and
  the element's own class tells you which one you are looking at.
- Buttons built by the shared `_button` factory take their name from a ternary,
  so the table marks them as _factory_; the rule lives under the factory in
  `player_controls.py`.

