# User guide

Mikai Ja Nai is an unofficial desktop client for the read-only Mikai Public API, with a separate local video player. You can browse the catalogue without downloading processing components, and you can play local files without catalogue access. Mikai is the service and API; **Mikai Ja Nai** is this application.

The player uses a native Qt Quick scene for its video and controls. Picture-in-picture is a compact presentation of that same player session; it does not open a second decoder. Settings and catalogue browsing remain in their existing application windows.

This guide gets you from opening the app to finding a title and choosing an episode. For focused instructions, see the [playback and Compare guide](player-guide.md), [processing and components guide](processing-guide.md), and [settings and privacy guide](settings-guide.md).

## Install and first launch

Use the [official GitHub releases page](https://github.com/smyhlin/Mikai-Ja-Nai/releases) for published artifacts; do not use unofficial mirrors. On Windows, either run the per-user setup installer or extract the portable ZIP and launch the app from its extracted folder. On Linux, choose the AppImage, dependency-based `tar.gz`, Arch `pkg.tar.zst`, or Debian `.deb` that matches your distribution and workflow. The Arch and Debian packages are installed with their distribution's package manager; the AppImage is run as an AppImage, and the tarball is unpacked and run. See the [Linux packaging notes](releasing.md#linux-x86_64), [distribution package details](releasing.md#distribution-packages-arch-debian), and [build-from-source guide](building.md) for platform prerequisites and packaging details.

Windows setup offers **Update** when it finds an existing installation registered for the current user. If both report the same version, setup can update or repair it but cannot tell which build is newer. Portable copies are not registered automatically; choose the intended folder explicitly if updating one.

Published artifacts are unsigned and each release provides `SHA256SUMS`. Release QA and third-party source/license obligations remain open; see [release status](release-status.md) and [redistribution notes](redistribution.md). When launched directly, the app may offer optional component setup; file launches do not show this offer. You can decline it and use ordinary playback.

The original Windows v0.1.0 packages can fail on startup with `__cxa_call_terminate` missing from `Qt6Core.dll`. The [player and usability update](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.1.0-update-20261101) includes the fix.

## Start here

Launch the installed or portable application. The left sidebar provides **Home**, **Filters** (with an active filter count badge when criteria are applied) and **Teams…**, with **Open video**, **Shortcuts**, **Settings** and **Check for updates** rows at the bottom. In narrower windows the sidebar becomes an icon rail: the brand name is hidden and tooltips name each action. **Home** returns to discovery, **Filters** opens the **Catalogue filters** dialog, and **Teams…** opens the **Teams Directory**.

**Open video** is a menu with **Video file…** and **Video URL…**. A chosen file or stream address opens in the player without using the catalogue, and it keeps working when catalogue requests fail; see [local video or URL playback](player-guide.md#local-video-or-url). **Video URL…** keeps a persistent history of recently opened stream addresses, so a previous URL can be reopened without retyping it (#41).

The search field (**Search by title…**, with its **Ctrl+K** shortcut keycap and autocomplete/history suggestions dropdown), **Search** and the **RPM** quota pill stay in the top toolbar while the Home blocks scroll below. Press **Ctrl+K** to focus the search bar from anywhere on the page, and press **Enter** to search. **Shortcuts** opens the keyboard-shortcut overlay; **Escape** closes transient overlays and returns to Home.

Below the search bar, a **Quick Categories** chip bar provides one-click genre jumps, a **Random** discovery shortcut, and quick access to **More filters**. The **Latest updates** and **Release schedule** rows keep their full caption and scroll sideways when a window is too narrow: **Shift + wheel** moves half a viewport, **Alt + wheel** scrolls smoothly, and a plain vertical wheel scrolls the page directly at a crisp step. The discovery feed shows a vertical scrollbar as needed and forwards vertical wheel gestures cleanly even when hovering over carousels or feed rows.

Home stacks **Recently opened** (titles opened on this machine), the **Currently airing** and **Popular on Mikai** carousels, then the **Latest updates** and **Release schedule** lists. Carousel arrows page by the visible cards, and **Alt + mouse wheel** over a carousel scrolls it horizontally; a plain wheel still scrolls the page. The four server-backed blocks can be switched off under **Settings → Access → Home page sections**, and a disabled block is not requested; **Recently opened** stays because it is local. Open a result to see its title page, releases and episodes. On a title page the genre and tag chips are links: a click opens the catalogue filtered by that genre or tag, replacing the current filters. Settings groups **Customization** (General, Appearance), **Application** (Access, Keyboard shortcuts) and **Playback** (Processing, Components, Upscalers, Benchmark); its footer holds **Save** (commits without closing, displaying an inline **Saved** indicator) and **Cancel**.

Catalogue browsing works anonymously, subject to the service's anonymous limits. A key is optional: enter it under **Settings → Access**, choose **Test**, then **Save** to use the higher keyed limit. Keep the key private; it is sent to the Mikai API. The Access page also carries the **API usage** readout. The [API reference](https://api.mikai.me/public/docs) explains the public API contract. Mikai Ja Nai targets Mikai Public API v1.5, whose Tier 1 and Tier 3 read-only endpoints power catalogue browsing, title pages, releases, episodes and team listings (#37).

The app can check for updates automatically and verifies downloaded release artifacts by SHA-256 before applying them, so an in-app update cannot silently replace the binary with a tampered build (#15). See [settings and privacy](settings-guide.md) for the update-channel controls.

The Home/Search header’s **RPM** pill reads **RPM: used / limit** from the last response that carried quota headers; its tooltip adds the remaining requests, the reset countdown and any queued requests. Its dot is neutral until quota data arrives, and the other tones mark healthy, low or exhausted remaining quota rather than a general connection-health check. The title page shows the same usage inline in its header. Both are shortcuts: pressing the pill (or the title page's inline line) opens **Settings → Access**, the page that owns the key the quota belongs to.

![Catalogue home (0.2.4.1) showing live Mikai titles and posters, sidebar, Ctrl+K search toolbar, Quick Categories chips, and request quota badge](images/gallery-2026-11-24/catalogue.png)

*Catalogue home (0.2.4.1, live Linux x86_64 capture with fetched Mikai titles and posters).*

## Find titles and filter the catalogue

1. Enter a title or search phrase in the top search field (or pick a suggestion from the dropdown), then press **Enter** or choose **Search**. The results header shows the active scope and formatted total (for example **Filtered catalogue · 1,341 titles**), active criteria appear as removable chips (`+ drama ×` with **Clear all**), and the results toolbar lets you switch between **Grid** (`360×140` `TitleCard` layout) and **List** views or change sort field and order. A compact **RangePager** below the results moves through pages and offers a page-size selector (**20 / 36 / 50 / page**). An empty search returns to Home when no catalogue filters are selected; when filters are selected it runs the filtered catalogue instead.
2. Open **Filters** (**Catalogue filters**) to narrow results. Its **Genres** and **Tags** sections use unified **tri-state chips** (click to cycle Neutral → `+ Include` → `− Exclude`); **Release** adds quick decade/year preset pills alongside year-from/year-to ranges, exact years, format, status, season and studio selections; **Licenses** lists official license holders from Mikai API v1.5; **Presets** saves and reloads reusable filter combinations locally; and **Sorting** sets the sort field and order.
3. For selected genres or tags, choose **Match all** or **Match any**. Include and exclude selections let you shape a result set—for example, include a genre and exclude a tag you do not want.
4. The **Apply** button shows a debounced live result-count preview as you edit filters. Choose **Apply** (or press **Ctrl+Enter**) to run the query, **Clear draft** to reset controls inside the dialog without closing it, **Reset** to clear and apply, or **Cancel** to discard edits.

![Filtered Search Results Grid (0.2.4.1) with live Mikai Action titles, active filter chip, format pills, 360x140 TitleCards and compact RangePager](images/gallery-2026-11-24/search-results-grid.png)

*Filtered Search Results Grid (0.2.4.1, live Action query). Active filter chips, full-width format pill tabs, Grid/List toggle, and compact RangePager.*

Examples: to look for recent television releases, pick a year preset pill and choose the relevant format. Use the sort controls to order by name, year, recently updated, views or MAL rating, ascending or descending; text searches use relevance ranking, so the selected sort may be retained but overridden. When results mix formats, a full-width row of format pill tabs with counts appears below the toolbar (**All (1,341)**, **TV (920)**, **MOVIE (245)**); choosing one filters the whole result set, and **All** removes that filter. Right-click a card or row for **Open**, **Copy link**, **Copy ID** and **Open in browser**.

If filter choices are missing because the catalogue dictionaries did not load, **Filters** offers **Retry dictionaries**. A failed catalogue request is not a playback failure: local-file playback remains separate from the API.

## Browse translation teams

Open **Teams…** to open the **Teams Directory**. Type in **Search teams** (debounced search), filter by **Voice** (`haveVoice`) or **Subtitles** (`haveSubs`), and sort ascending or descending. Scrolling to the bottom automatically loads additional pages. Checked teams appear in a persistent **Selected teams** chip strip with **Clear selected**; choose **Filter catalogue** to apply them as catalogue criteria.

Highlight a row (or double-click / press **Enter**) and choose **Browse anime** to open that team's **Team Profile**: a structured hero card with a monogram badge, **Teams › Team Name** breadcrumb navigation, horizontal social/website/donate links, locale-formatted view and anime totals, a **Ukrainian Posters** gallery, and in-team search, sort, and format filtering. You can also jump directly to any team's profile by clicking its pill in the **Teams:** row on a title page.

## Choose a title, release and episode

![Live Title Page (0.2.4.1) with poster, synopsis, Dubbed chips, 32px Provider tabs, clickable Teams pills and episode grid](images/gallery-2026-11-24/title-page.png)

*Title Page (0.2.4.1, live Linux x86_64 capture showing poster, synopsis, Dubbed chips, Provider tabs, clickable Teams pills, and episode list).*

Open a catalogue result to view its title page. The header has **Back** (returns to the previous title page or the catalogue view you came from), **Home**, the inline API-usage line and the API-token gear.

Click either displayed title name to copy that name to the clipboard. The alternate title line appears only when a distinct alternate name is available. You can also focus either name with **Tab** and press **Enter** or **Space** to copy it.

The hero offers **Watch**/**Continue** and **More details** (the button reads **Hide details** while the sheet is open). **Watch**/**Continue** plays the episode shown in the selected-episode panel and is disabled when that entry has no playable video. **More details** opens a dismissible sheet with the fact grid, synopsis and links, including the title's page on Mikai; close it with its close button, by clicking outside it, or with **Escape**. **Read more** expands the synopsis in place and then reads **Show less**. The numbered episode section remains below the release and provider selectors; the title details sidebar's **Episodes** link jumps to that section. Genre and tag links use compact, low-emphasis outlines below the primary actions. All retain the global corner-radius setting.

A title can have several releases, grouped by type (for example TV series or movie) under **Release**. Each chip names the translation team (or the release type when no team is listed) and the playable episode count, marks collaborations, and adds a probed resolution once one is known (for example **Team · 12 · 1080p**). Right-click a chip for **Browse <team> releases** to open that team's catalogue page. Release types are separated into labelled lanes; the **Provider** selector is visually divided from the subtitle choices. Each provider tab is one source; MoonAnime is ordered first when a release provides it, and the title opens on the release and provider that carry it, otherwise on the first entry. Choosing another release or provider keeps that choice while you stay on the title.

Episodes appear as numbered buttons under **Episodes**, grouped into ranges of up to 140. Choose a range, a subpage of up to 52 episodes, or **Show entire range**. Enter an episode identifier and press Enter to jump to it. When the selected provider's episode entries include classifications, coloured dots identify the types shown in the filter legend; hover an episode for its classification. **All** clears the filter. A separate legend explains **Current**, **Watched** and **Unavailable**. When the API reports a total episode count, numbers a provider does not supply appear as disabled buttons. Clicking a playable episode opens it in the player; it does not change the release or provider.

**Unavailable** means the API marks that episode unavailable; **No video available** means the app has no playable video for that entry/provider. The app may only learn the latter when it asks for quality options, after which the buttons for that stream are disabled, so a listed episode is not a guarantee of a working stream. If the API marks the title or its player data as licensed, the title page reports playback unavailable due to licensing and shows no releases. If no video is available or a stream request fails, try another episode, provider or release where available. If catalogue requests fail, check the connection and, if you use a key, test it under **Settings → Access**. Provider and playback details are in the [playback guide](player-guide.md).

Explicit content is allowed by default. With **Settings → Appearance → 18+ content** turned off, a title rated 18+ asks **This title is rated 18+. Start playback?** before its first playback, and accepting covers the rest of that title.

The **More to explore** section groups **Related titles**, **Similar anime** and **Recommended on Mikai**, paged with **Previous page**/**Next page** controls; **Hide** collapses the section and **Show** reopens it. Opening a title there navigates to its title page, and **Back** returns.

## Keep track of episodes

Watched status and in-progress position are stored locally, not synchronized to the API. On the title page the legend and episode buttons mark the current and watched episodes, and the selected playable episode's panel shows its provider tab, any probed resolution, the saved position and a progress bar. Use the panel's **Mark as watched**/**Watched** toggle, or right-click a playable episode for **Copy to clipboard** (the page URL, then a direct link for each quality once loaded), **Mark as watched**, **Unwatch**, **Watch all previous**, **Unwatch all previous**, **Open in external player** (the best quality in mpv or VLC) and **Open in browser**. Unavailable entries, and entries the app has found no video for, are disabled and do not offer the episode menu.

Playback opens a player that carries the current provider's episode list (**Episode list (S)**) with **Previous episode** and **Next episode** arrows. Changing episode inside the player updates the title page's current episode, and finishing an episode marks it watched. A saved position is recorded from 30 seconds onward. At 90% of a known duration the episode is marked watched, and finishing also marks it watched; watched episodes do not resume. When a position is saved the player presents a themed prompt offering **Continue** or **Start over**, and **Start over** clears that episode's saved offset. These are local catalogue-history rules, not a promise of resume for local video files; see the [playback guide](player-guide.md#episode-resume-and-watch-state) for the resume prompt and player controls.

## Local state and privacy

The Home **Recently opened** list, watch history and settings are local application data; they are not account-synced to Mikai. API requests go to the Mikai service, and an optional API key is sent with those requests. Do not share or commit your key. For the key controls, other settings, and local-data details, see [settings and privacy](settings-guide.md).

## Compare and export

For frame comparison and GIF, WebP or MP4 export, see the [playback and Compare guide](player-guide.md#compare-two-processing-results). The player’s **Copy frame** action copies the presented picture to the clipboard. To make a short clip from original source media, follow [Cut a moment](player-guide.md#cut-a-moment). Compare and Clip open as auxiliary windows; PiP remains in the same player window.

## Optional video processing and components

Optional processing, model/runtime components and their limitations are covered in the [processing guide](processing-guide.md#optional-video-processing-and-components). Ordinary catalogue browsing and Original playback do not require AI processing components.

## Benchmarking

Benchmark setup, required clips, report locations and interpretation are covered in the [processing guide](processing-guide.md#benchmarking).
