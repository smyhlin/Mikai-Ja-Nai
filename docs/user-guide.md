# User guide

Mikai Ja Nai is an unofficial desktop client for the read-only Mikai Public API, with a separate local video player. You can browse the catalogue without downloading processing components, and you can play local files without catalogue access. Mikai is the service and API; **Mikai Ja Nai** is this application.

This guide gets you from opening the app to finding a title and choosing an episode. For focused instructions, see the [playback and Compare guide](player-guide.md), [processing and components guide](processing-guide.md), and [settings and privacy guide](settings-guide.md).

## Install and first launch

Use the [official GitHub releases page](https://github.com/smyhlin/Mikai-Ja-Nai/releases) for published artifacts; do not use unofficial mirrors. On Windows, either run the per-user setup installer or extract the portable ZIP and launch the app from its extracted folder. On Linux, choose the AppImage, dependency-based `tar.gz`, Arch `pkg.tar.zst`, or Debian `.deb` that matches your distribution and workflow. The Arch and Debian packages are installed with their distribution's package manager; the AppImage is run as an AppImage, and the tarball is unpacked and run. See the [Linux packaging notes](releasing.md#linux-x86_64), [distribution package details](releasing.md#distribution-packages-arch-debian), and [build-from-source guide](building.md) for platform prerequisites and packaging details.

The published artifacts are unsigned. Each release provides `SHA256SUMS`; further public binary distribution is on hold while release QA and third-party source/license obligations remain unresolved. See [release status](release-status.md) and [redistribution notes](redistribution.md); publication does not imply rights clearance or release approval. When launched directly, the app may offer optional component setup; file launches do not show this offer. You can decline it and use ordinary playback.

The published Windows v0.1.0 packages can fail on startup with `__cxa_call_terminate` missing from `Qt6Core.dll`. A source fix and locally rebuilt packages are recorded in [release status](release-status.md); they do not imply that a corrected public release has been published.

## Start here

Launch the installed or portable application. Home offers discovery feeds; Search is where you can search and filter the catalogue; **Teams…** opens a team picker. Open a result to see its title page, releases and episodes. Press **Enter** in the search field to search; **Escape** returns to Home (or dismisses the keyboard-shortcut overlay first). The **Keyboard shortcuts** button opens the app's shortcut overlay. Settings contains API access, appearance, playback and processing options.

Catalogue browsing works anonymously, subject to the service's anonymous limits. A key is optional: add one under **Settings → Access** to use the higher keyed limit. Keep the key private; it is sent to the Mikai API. The Access page provides a token test and API-usage readout. The [API reference](../docs.md) explains the public API contract.

## Find titles and filter the catalogue

1. Open **Search**, enter a title or search phrase, then press **Enter** or choose **Search**. Clear the field and search again to return to the unfiltered catalogue browse; if catalogue filters remain selected, they still apply.
2. Open **Filters** (the dialog is titled **Catalogue filters**) to narrow results. It provides genre and tag include/exclude lists; year-from/year-to ranges and a comma-separated list of exact years; format, status, season and studio selections; and sort field/order. The search fields narrow the displayed genre, tag and release-option lists (formats, statuses, seasons and studios).
3. For selected genres or tags, choose **Match all** or **Match any**. Include and exclude selections let you shape a result set—for example, include a genre and exclude a tag you do not want.
4. Choose **Apply** to apply the selections, or **Reset** to clear catalogue criteria. Both actions keep the search text. **Cancel** discards edits made in the dialog.

Examples: to look for recent television releases, set a year range and choose the relevant format. Use the sort controls to order by name, year, recently updated, views or MAL rating, ascending or descending. Text searches use relevance ranking, so the selected sort may be overridden. Search results may also show format tabs when the results contain multiple formats; choose a format to filter, or **All** to remove that format filter. Results reflect catalogue data and selected criteria; an empty result does not establish that a title is unavailable from every source.

Team selection is handled in **Teams…**, not in the catalogue-filter dialog: check one or more teams there and choose **Filter catalogue** to apply them. If filter choices are missing because catalogue dictionaries did not load, open **Filters** and choose **Retry dictionaries**. A failed catalogue request is not a playback failure: local-file playback remains separate from the API.

## Browse translation teams

Open **Teams…**, enter a name in **Search teams**, and select a row. Choose **Browse anime** to see titles from that team; **All catalogue** leaves that team view and returns to catalogue results. Team search is paged; use **Load more** to fetch another page. Check one or more rows and choose **Filter catalogue** to apply those teams as catalogue criteria instead of opening one team's results. A failed team request is shown in the picker or page status; retry by searching again or reopening the picker.

## Choose a title, release and episode

Open a catalogue result to view its title page. A title can have multiple releases (for example, from different translation teams); a release can expose separate provider tabs for its episodes. Select the release and provider, then choose an episode. **Unavailable** means the API marks that episode unavailable; **No video available** means the app has no playable video for that entry/provider. The app may discover the latter when it requests quality options, so a listed episode is not a guarantee of a working stream. If the API marks the title or player data as licensed, the title page reports playback unavailable due to licensing and does not show releases. Catalogue presence does not guarantee that every release or episode has a working stream.

For a playable episode, choose **Watch** (or **Continue** if a saved offset is available). Mikai Ja Nai requests stream information from the selected provider; availability and extraction depend on that provider. If no video is available or a stream request fails, try another episode, provider or release where available. If catalogue requests fail, check the connection and, if you use a key, test it under **Settings → Access**. Provider and playback details are in the [playback guide](player-guide.md).

## Keep track of episodes

Watched status and in-progress position are stored locally, not synchronized to the API. On a title page, the episode list marks watched episodes; the selected playable episode's panel can show its saved elapsed position/progress. Select an episode to choose it; use the panel's **Mark as watched**/**Watched** toggle, or right-click a playable episode and choose **Mark as watched** or **Unwatch**. Unavailable or no-video entries cannot be watched or manually marked.

A saved position is recorded from 30 seconds onward. At 90% of a known duration the episode is marked watched, and finishing also marks it watched; watched episodes do not resume. Choosing **Start over** in the player clears that episode's saved offset. These are local catalogue-history rules, not a promise of resume for local video files; see the [playback guide](player-guide.md#episode-resume-and-watch-state) for the resume prompt and player controls.

## Local state and privacy

Recent titles, watch history and settings are local application data; they are not account-synced to Mikai. API requests go to the Mikai service, and an optional API key is sent with those requests. Do not share or commit your key. For the key controls, other settings, and local-data details, see [settings and privacy](settings-guide.md).

## Compare and GIF export

For frame comparison and GIF export, see the [playback and Compare guide](player-guide.md#compare-two-processing-results).

## Optional video processing and components

Optional processing, model/runtime components and their limitations are covered in the [processing guide](processing-guide.md#optional-video-processing-and-components). Ordinary catalogue browsing and Original playback do not require AI processing components.

## Benchmarking

Benchmark setup, required clips, report locations and interpretation are covered in the [processing guide](processing-guide.md#benchmarking).
