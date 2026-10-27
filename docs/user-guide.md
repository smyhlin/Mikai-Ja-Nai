# User guide

Mikai is an unofficial graphical client for the read-only Mikai Public API and an independent local video player. Catalogue browsing and local-file playback are distinct: an API outage can affect catalogue features without preventing playback of local files. Provider stream availability is controlled by the catalogue and provider, not by the player.

## Start using Mikai

Launch the installed or portable application. API access works without a key under the service's anonymous limits. An optional key increases the API limit and is configured in **Settings → General**; keep it private. The key is sent to the Mikai API, not bundled into the program. The [API reference](../docs.md) describes its public contract.

Use Home and the discovery carousels to browse, or open Search and filter by text, genre/tag, year, season, format, status, studio, translation team and sort order. Open **Teams…** to browse teams and their catalogue. A title page displays available releases and episodes; choose a playable episode to request its provider stream. Recent titles and watch history are local application data, not synchronized to the API.

## Playback

### Catalogue streams

Select **Watch** on a title or an episode from its release grid. Stream extraction is performed for the selected provider; availability and extraction behavior vary by provider. The player uses the integrated libmpv engine. Standard playback begins with **Original** output. Anime4K, AnimeJaNai and Native processing are optional and are only enabled after an explicit selection.

Player controls include a timeline with thumbnail/chapter scrubbing, audio and subtitle track menus, an episode drawer (`S`), picture-in-picture, fullscreen, and multi-level `Esc` navigation. The `+85` seek control advances by 85 seconds, bounded by known duration. Normal end-of-file marks a catalogue episode watched; failed loads and early closes do not imply completion. Episode context actions can mark one as watched or apply watch/unwatch actions to earlier episodes in the provider's catalogue order.

### Local files

Open a file from the main window/player or drag a supported video onto the main window/player. Dropping on the main window opens a standalone player; dropping onto an existing player replaces its media. Mikai does not move or delete the source file. File launches do not show the startup offer for AI component setup.

Local playback discovers embedded audio/subtitle tracks and matching sidecars in the exact-match mode; unrelated files in the directory are not loaded. Select tracks from **Audio / Dubs** and **Subtitles**. Use the player's context menu for the local media information inspector: **Overview**, **Tree**, and selectable/copyable **Plain text** views share metadata from the active playback session.

On Windows, **Set as default video player…** registers Mikai as an eligible per-user application and opens Windows Default Apps. You must confirm associations there; if Windows does not open Default Apps, the candidate registration remains, but no associations have changed. **Settings → General → System integration** offers opt-in launch-at-sign-in and Explorer **Open with Mikai** actions. Changes apply on Save; Cancel leaves the system unchanged. Mikai does not force file associations or write protected Windows `UserChoice` values. Disable integrations before moving a portable folder. If a saved launch-at-sign-in entry points to an old location, remove the stale entry through Windows Startup Apps or from the old installation, then enable it again. Mikai does not take over unmarked Explorer verbs belonging to another installation or program; an ownership conflict is reported rather than silently adopted.

On Linux, **Settings → General → System integration** offers opt-in XDG launch-at-sign-in and desktop **Open With** registration; changes apply on Save, not Cancel. Open With makes Mikai an application candidate, not a Windows Explorer-style context-menu verb. The default-player action requests the desktop's default-app settings; choose associations yourself, as Mikai does not force MIME defaults. Desktop support varies. Disable integrations before moving a portable folder.

Picture-in-picture depends on the window manager. On Wayland, the compositor controls interactive moving/resizing and may not honor always-on-top. Use the restore button to return to the player window; a window-manager close follows the same player-owned close path as PiP's close button instead of leaving hidden playback behind.

## Compare and GIF export

Open **Compare** from the player while a video is active to inspect candidates against the same captured frame. Modes include horizontal/vertical split, side-by-side, flicker and difference; candidates include Original, Anime4K, AnimeJaNai and prepared Native options when available. Use `S` to swap sides, `R` to capture again, `F` to fit, `P` to pin, `F11` for fullscreen, `Space` to peek Side A, mouse-wheel zoom and drag to pan. `Esc` exits fullscreen or closes the window.

Save GIF is available in Flicker mode. It exports the settled pair as two infinitely looping, aspect-preserving frames. GIF's fixed 256-colour palette quantizes colors: it is a visual convenience, not a bit-exact comparison. Export is atomic and write errors are reported.

## Optional video processing and components

Anime4K shaders use the OpenGL rendering path and need no AI model download. AnimeJaNai and prepared Native processing use separately acquired components/models, may require a supported GPU/runtime, and can consume substantial disk, memory and GPU resources. They are not required for ordinary playback. Do not interpret model readiness or an imported ONNX file as proof that the graph will execute on a given backend.

On Linux, Standard AnimeJaNai uses TensorRT on NVIDIA hardware and requires a compatible AnimeJaNai fork of libmpv plus compatible CUDA and TensorRT components. The app pins separate Windows and Linux mpv runtime archives. The `mpv-fork` source entry still records ABI-8 commit `63d127174d8f70336c82a22408929e2bc301edcb`; the Windows archive has separate source commit `b4f9f5bfed18d5ce610af28024986410dd9bdaae`, while Linux archive build/source correspondence is not established. Do not infer GPU inference from archive identity. The official v0.17.1 runtime is API/ABI 9; historical official v0.9.0 ABI-8 and separately patched private MSVC AJI ABI-8 builds are distinct cohorts and cannot be substituted. DirectML and the project's private Native backend are Windows-only. The current portable layout stores models and acquired components in executable-adjacent `native/`, which must be writable for AI acquisition and use. A read-only system installation needs a user-owned writable deployment; do not change permissions on system directories to work around this. Ordinary Original playback does not need an AI download. The separate approximately 80 MB Linux `aji-encode` archive is an optional offline encoder, not a playback dependency, and is neither automatically downloaded nor adopted by the app; its FFmpeg dependency and redistribution rights are unresolved.

The initial setup offer appears only on direct application startup. Its recommended selection omits optional RIFE frame interpolation by default. Settings → Components lets you inspect/acquire selected runtime and model packages; verify the displayed state before using a preset. Downloads are verified before installation and are kept in the application's native data area. Closing Settings does not cancel an active acquisition; use Cancel on its activity UI.

For AnimeJaNai custom models, open **Settings → Processing** and select **Standard**. Import/register a self-contained spatial ONNX graph through **Import .onnx Model…**; this records integrity metadata in the application-owned model registry. Imported files and external registrations are distinct: external files are borrowed, not copied or modified. A successful import/registration checks structure and provenance but does not establish backend, shape, dtype, operator or GPU compatibility. Removing an imported model also removes owned staged artifacts; removing an external registration does not delete its graph.

Profiles may combine ONNX and Anime4K stages and optional RIFE. Chain selection uses source geometry/FPS metadata. Settings → Processing exposes temporal frame count (Auto or 1/odd 3–15), scene-detection threshold, upscale enable/disable and temporal-fix controls. Official v0.17.1 source maps RIFE code 4262 to `rife_v4.26`, not the heavy model; this source mapping is not evidence of inference. The TensorRT 11/CUDA 13 component closure follows the current backend dependencies. Custom slots and model/profile changes are managed in Settings → Processing. Profile export formats (`.pconf` or `animejanai.conf`) do not embed model graphs, runtime components or compiled engines. TensorRT engines are GPU/runtime-specific. Mixed AJI/Anime4K chains can transfer frames through software NV12; they are not guaranteed zero-copy or HDR-preserving.

Prepared Native bundles are a separate workflow, not aliases for AnimeJaNai profiles or private API status/admission protocols. Read [Native documentation](../native/README.md) and its [MSVC build notes](../native/build-msvc.md) before preparing or distributing a bundle.

## Benchmarking

Settings → Benchmark measures selected clips/candidates through the app's player/render path. Benchmark clips are not redistributed: supply `480x360.mp4`, `640x480.mp4`, `768x576.mp4`, `1280x720.mp4` and `1920x1080.mp4` in `benchmarks/` beside the executable, with matching dimensions and exact filename case. On Linux, JSON reports are written under Qt's writable `AppLocalDataLocation/bench` (typically `~/.local/share/Mikai/Mikai Wrapper/bench`, depending on XDG settings); on Windows they are under `bench/` beside the executable. Reports include model identifiers, measurements or failure details, source/target geometry and runtime/environment metadata. Compilation time is separate from sampling. A failed runtime/setup row is not a throughput result.

Benchmark results depend on GPU, driver, runtime, clip, output size, cache/engine state and foreground/display conditions. The published captures in [benchmark results](../bench_results.md) are historical, not promises of real-scene playback speed; some use synthetic black-frame fixtures, which do not represent anime content. Consult the limitations and provenance before comparison or publication.

## Appearance, language and settings

Settings → Customization controls interface appearance and content/name languages. UI language offers English and Ukrainian; it can preview in existing windows and is saved only when you choose Save. Cancel, Escape or closing Settings restores the opening choice. Content-title and name language are separate from interface language. Theme, accent, corner radius and gradients preview while the dialog is open; Save persists the changes and Cancel restores the prior appearance.

The app stores native settings under organization `Mikai`, application `Mikai Wrapper` for compatibility. Recently opened titles are in application settings; they are not a `recent.json` file. Application logs and crash reports are adjacent to the executable. Native/model data lives under `<applicationDir>/native`; runtime acquisition cache uses `%LOCALAPPDATA%/MPVBackend/runtime` on Windows, or `$HOME/MPVBackend/runtime` if `LOCALAPPDATA` is unavailable.

**Save** applies settings stores independently rather than as one all-or-nothing transaction. Cancel before Save does not write the startup or Explorer integrations and restores language/appearance previews. If a later store fails during Save, earlier stores that succeeded (for example, API credentials or native configuration) remain saved; a subsequent Cancel only discards unsaved edits and restores previews. System-integration changes have their own rollback attempt, but an operating-system error can prevent a complete rollback; the app reports such failures.

Optional `.env` configuration may contain `API_KEY=<token>`; never commit it. The API key may instead be set in Settings → General. See the repository [security guidance](../SECURITY.md) and the [API reference](../docs.md) for authentication details.
