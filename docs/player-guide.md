# Player guide

This guide covers the player window, catalogue episodes, local videos and URLs. Catalogue playback is resolved through the selected provider; local playback is a separate file-player workflow. For processing and component setup, see the [processing guide](processing-guide.md).

## Start playback

### Catalogue episode

1. Browse or search the catalogue, open a title, and choose a playable episode from its release/episode list.
2. Select **Watch** (or **Continue** if a saved position exists). The provider supplies the stream; provider availability and extraction behavior can vary.
3. Use the player’s **Audio** and **Subtitles** controls (or **T** and **C**) to choose among the title’s available provider variants. These change the selected stream variant, not an embedded track in one local file.

The catalogue owns the episode list. Open it with **S**, choose an episode to switch, or use the previous/next episode controls when available. Those controls are shown only when another episode exists in the playlist. Episode-context actions can mark an episode watched or mark earlier episodes watched/unwatched in the provider’s catalogue order.

### Local video or URL

From the main window, choose **Open video → Video file…** and select a file. The picker lists common video extensions and **All files**. **Open video → Video URL…** accepts a video or stream URL. You can also drag a video file onto the main window to open a standalone player, or onto an existing player to replace its media.

The player does not move or delete a local source file. Local media can have embedded audio/subtitle tracks and exact-match sidecars; unrelated files in the containing folder are not indiscriminately loaded. Use **Audio** and **Subtitles** to choose available tracks; the local subtitle menu includes **Off**. Track rows may include title, language, codec, channels and flags such as external, forced or default. A menu may report that no alternate audio or no subtitles are available when the source has no choices.

Catalogue variant selection and local track selection are different operations: a catalogue choice selects a provider variant, while a local-file choice selects a track in the active media.

## Transport and timeline

- **Space** toggles play/pause. The player dock also has a play/pause control.
- The timeline shows playback position and remaining time when duration is known. Drag/scrub it to seek; hovering shows the time at that point, not a thumbnail preview.
- The separate **+85** control advances by 85 seconds, bounded by a known duration.
- **← / →** seeks backward/forward by 5 seconds when duration is known. If duration is unknown, those keys request the previous/next episode instead.
- **↑ / ↓** changes volume by 10 percentage points; **M** toggles mute.
- **T / C / Q** open Audio, Subtitles, and Quality and speed, respectively. Menus can be navigated with **↑ / ↓** (or **← / →**), confirmed with **Enter**, and closed with **Esc**.

## Keyboard, mouse and window navigation

The player’s **?** or **/** shortcut overlay lists the available keys:

| Key | Action |
|---|---|
| `Space` | Play / pause |
| `F` or `F11` | Fullscreen / windowed |
| `M` | Mute / unmute |
| `S` | Episode list |
| `T` | Choose audio track/variant |
| `C` | Choose subtitle track/variant |
| `Q` | Quality and speed |
| `←` / `→` | Seek −/+ 5 seconds when duration is known; otherwise request previous/next episode |
| `↑` / `↓` | Volume ±10% |
| `Esc` | Dismiss the topmost open panel/overlay; when none remain, close the player |

A left-button double-click on the player picture also toggles fullscreen/windowed. **Esc** is a sequence, not an immediate close: open panels are dismissed first. The picture context menu provides **Copy frame**, **Compare quality**, **Copy link**, **Open containing folder** when applicable, **Media information** for local media, **Keyboard shortcuts**, and **Exit**.

## Episode resume and watch state

Catalogue episode progress is saved locally, not synchronized to the Mikai service. Playback starts at the beginning; when a saved position is available and the duration is known, the player offers **Continue** or **Start over**. Continue seeks to the saved point. Start over clears the saved offset. Dismissing the prompt leaves the offset available for a later attempt.

Progress updates below 30 seconds do not create or replace a resume point. At 90% of a known episode duration, the episode is marked watched; an episode that finishes also becomes watched. Finishing clears its saved offset. On the title page, an unfinished saved position can appear as a progress bar and elapsed/total text, and the action changes to **Continue**. The title-page **Watched** control can manually mark that episode watched or unwatched. These thresholds describe catalogue episode history, not a local-file resume promise.

## Picture-in-picture

Choose the player’s PiP control to open a frameless, always-on-top floating player; choose **Restore** to return to the main player window. The float uses its own playback surface for the same stream, while the original surface is paused. PiP exposes its own transport and volume controls.

PiP behavior is window-manager dependent. On Wayland, the compositor controls interactive movement/resizing and may not honor always-on-top. Closing the float closes the player rather than silently leaving hidden playback behind.

## HDR and media badges

In **Settings → Application → Access → HDR playback**, choose **Auto (recommended)**, **Stable – no per-frame adaptation**, or **Vivid – Mobius curve**. Saved tone-mapping changes reach a running player shortly afterward. This controls how HDR is mapped to the display; it is separate from Native enhancement.

The separate **Native enhancement for HDR** checkbox permits processing that is normally skipped on HDR sources. The required stages use 8-bit NV12; enabling the override does not convert the source to SDR and may reduce HDR image quality. Unknown transfer metadata is not proof that a source is SDR, so this override should not be described as enabling processing for unknown sources.

Player badges such as **HDR**, **DV**, **ATMOS**, and the codec name reflect metadata reported by the source. A missing badge means the container did not report that metadata; a badge does not verify the display’s output. Processing chains are not guaranteed to preserve HDR; see the [processing guide](processing-guide.md) for processing limitations.

## Media information for local files

For a local file, open the picture context menu and choose **Media information**. The inspector has **Overview**, **Tree**, and **Plain text** views, based on the active playback session. It reports metadata such as filename/path, container, duration, file size, bitrate, dimensions, frame rate and video codec, plus available audio/subtitle track information. Information is gathered asynchronously; unavailable fields/errors are shown rather than guessed. This menu action is offered for local media, not as a general catalogue-stream inspector.

## Compare two processing results

With a video active, choose **Compare quality** from the player’s picture context menu. A loading indicator appears at the center of the player while Compare opens. Compare captures the current playback frame and lets you render/select two candidates against that same frame. It holds playback paused while the comparison is open, then resumes it on close only if it was playing beforehand. Use each side’s selector to choose candidates; comparison is for inspecting outputs at a captured position, not a live second playback.

Choose a view in the mode menu:

- **Split H** and **Split V** show A and B on opposite sides of a horizontal or vertical divider. Drag the divider to change the split; double-click it to return to the midpoint.
- **Side-by-side** shows each candidate in its own pane.
- **Flicker** alternates between A and B. Its rate control applies in this mode.
- **Difference** shows the visual difference between the candidates; its gain control applies in this mode.

Choose **Bilinear** or **Nearest Neighbor** for scaling. The mouse wheel zooms around the pointer. Drag the picture to pan; double-click the picture away from the split divider fits the image. **Fit** (or **F**) fits the view. **S** swaps A and B; **R** captures the current player frame again; **P** pins/unpins the Compare window so it stays on top. Hold **Space** to peek at Side A; releasing it restores the selected comparison view, including its zoom and pan. **F11** toggles Compare fullscreen. **Esc** leaves fullscreen first, otherwise closes Compare.

**Export** is available in Flicker mode when both current A/B renders are ready. Choose an output folder, file name and format: looping GIF, looping animated WebP, or a silent ten-second MP4. GIF uses an adaptive palette of at most 256 colors per frame; use WebP or MP4 when more color detail is needed. MP4 uses Motion JPEG, which may not play in every browser or video player. A short-lived completion window can open the file or its folder, or copy the file to the clipboard. Exports are written atomically; an error or cancellation does not publish a partial file.

## Practical troubleshooting

- **No alternate audio/subtitle choice:** check whether the source actually has multiple audio tracks or any subtitles. For a catalogue stream, provider variants are the choices; they are not necessarily embedded tracks.
- **A local sidecar is missing:** confirm its name matches the media under exact-match discovery; files elsewhere in the folder are not loaded just because they are nearby.
- **No resume prompt:** catalogue history ignores positions below 30 seconds, and watched episodes do not resume. Local media is not covered by the catalogue’s watch-history thresholds.
- **HDR badge or output does not match expectations:** badges reflect source metadata, not display behavior. Check the HDR playback policy; remember that Native HDR override may reduce image quality.
- **PiP cannot be moved/resized or stay above other windows:** compositor/window-manager policy can constrain it, especially on Wayland; use **Restore** to return to the player.
- **Compare does not offer Export:** Export is Flicker-only and disabled until both selected renders are ready. If it fails, read the displayed error and choose a writable destination.
