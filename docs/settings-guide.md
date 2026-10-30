# Settings guide

Open **Settings** from the main window. The settings window has these pages:

| Group | Page | Use it for |
|---|---|---|
| **Customization** | **General** | Site theme, content/name language, cover display, 18+ display, and system integrations |
| **Customization** | **Appearance** | Accent, corner radius and background gradient |
| **Application** | **Access** | Mikai API token, home-page feed sections, interface language and HDR playback |
| **Application** | **Keyboard shortcuts** | Anime4K preset key bindings |
| **Playback** | **Processing** | Playback runtime, AnimeJaNai and Native processing options |
| **Playback** | **Components** | Optional runtime/model component inventory and downloads |
| **Playback** | **Benchmark** | Rendering throughput comparison |

The page contents scroll independently; the **Save** and **Cancel** buttons stay in the Settings footer. For detailed processing, component installation and benchmark instructions, see the [processing guide](processing-guide.md).

## Change appearance and display preferences

In **Customization → General**:

- **Site theme** switches between dark and light.
- **Content titles language** selects Ukrainian, English or original titles.
- **Names language** selects **As the interface**, Ukrainian, English or original names.
- **Gradient overlay** controls the gradient drawn over cover images.
- **18+ content** shows explicit content without a warning. Turn it off if you do not want that display behavior.

In **Customization → Appearance**:

- Choose an accent-color swatch or use the custom color picker.
- Choose a corner radius. This affects the app's rounded controls and player chrome as well as the rest of the interface.
- Toggle **Background gradient** to show or hide the accent glow at the top of the page. When it is off, its intensity, height and color controls are disabled. When on, adjust intensity and height with the sliders and choose a preset or custom gradient color.
- **Reset** resets the appearance-page choices (accent, radius and background-gradient settings). It does not reset the separate General-page choices.

Appearance and site/content display changes preview while Settings is open. **Save** keeps them; **Cancel**, Escape, or closing the dialog restores the appearance from when that Settings session opened.

## Interface language, token and feeds

In **Application → Access**:

### Interface language

Choose **Use system language**, **English** or **Ukrainian**. A new choice is applied immediately to open application windows, including an open player; it is only a saved preference after **Save**. Cancel or closing Settings restores the language that was active at opening. If the requested translation catalog cannot be applied, the language remains unchanged and the selector returns to the installed language with an error message.

The interface language is independent of the title and name language choices under **Customization → General**.

### Mikai API access token

The app can make anonymous API requests; the token is optional. The Access page says a key raises the request limit from 60 to 300 per minute and is stored locally in a `.env` file. Use **Test** to check the text currently in the field; testing does not itself save it. Get a key from the linked Mikai developer dashboard. Keep the token private: it is sent to the Mikai API, and any `.env` file containing it should not be committed or shared. The app writes the key as `X-API-Key: …`; on Windows it restricts the file to the current user, and on Linux it grants read/write access only to the file owner.

The client first checks `.env` in the current working directory, then beside the executable, then in its immediate parent directory; the first existing file wins, and it does not search farther up. If no file exists, saving creates `.env` in the current working directory. At client initialization, a non-empty `MIKAI_API_KEY` environment variable takes precedence over the file. If saving the key fails, the app reports the error and leaves the active key unchanged.

The **API usage** readout shows the current limit and, after API responses arrive, usage/remaining requests, active and queued work, and reset countdown where available. It may initially say it is waiting for the next response.

### Home page sections

Check the home-page sections you want loaded. Disabled sections are not loaded and do not use API requests. Search and title pages remain available regardless. Feed choices are saved on **Save**.

## HDR playback

The **HDR playback** controls are on **Application → Access**:

- **Auto (recommended)** selects the automatic tone-mapping policy.
- **Stable** disables per-frame adaptation.
- **Vivid** uses a Mobius curve.
- **Native enhancement for HDR** allows Native enhancement stages on HDR video. Those stages require 8-bit NV12; enabling this option does not convert the source to SDR and may reduce HDR image quality.

These settings are separate: HDR tone mapping describes how HDR video is mapped to a display, while Native enhancement is a processing choice. Saved HDR changes reach a running player shortly after saving.

## Anime4K preset keyboard shortcuts

Open **Application → Keyboard shortcuts**. Focus a shortcut field and press a key or a combination using Ctrl, Alt or Shift. Use the field's clear button to remove one binding. **Clear all** clears all preset bindings in the page; the filter matches preset names and key text and does not change bindings.

A key combination can belong to only one preset, and built-in player shortcuts are reserved. A conflicting edit is rejected and the page explains the conflict; saving is also blocked while the preset bindings are invalid. Bindings are written on **Save**, not as soon as you type them.

## Windows and Linux system integrations

Find these controls under **Customization → General → System integration**. They apply to the current user account and take effect on **Save**. They do not take effect just by checking a box.

| Option | Windows | Linux |
|---|---|---|
| Start at sign-in | Registers the app for the current Windows user. | Registers the app for the current user using XDG autostart. |
| Open With | Adds an **Open in Mikai Ja Nai** video-file context-menu command without changing the default app. | Registers the app as a video-file **Open With** candidate; file managers decide where it appears. |

Neither platform option forces file associations. On Linux, the desktop decides how default-app settings and Open With entries are presented. If default-player registration was requested, disabling Open With keeps the desktop identity so existing defaults can continue to refer to it; a package-managed entry cannot be disabled in the app. On Windows, an existing unmarked context-menu registration or one owned by another installation is left unchanged rather than adopted.

Before moving a portable application folder, disable its startup and Open With integrations, move it, then enable them again from the new location so registered paths can be updated. If the move happened first, a stale Windows startup entry may need to be removed from Windows before enabling startup at the new location. If saving an integration fails, Settings stays open with the edits. The integration writer attempts rollback; the app reports if rollback itself could not restore the previous state. On platforms other than Windows and Linux these controls are unavailable.

The player also has a **Set as default video player…** action in its **Quality and speed** flyout. It registers Mikai Ja Nai as a per-user candidate and opens the Windows or desktop Default Apps settings; it does not change defaults automatically, so choose Mikai Ja Nai and the video file types there. Registration refuses conflicting entries it does not own, and if a registration write fails it attempts to restore the prior values. Candidate registration can succeed even if opening the OS settings fails. In that case, the error gives the manual route; the registration remains and can be used when choosing a default later.

## Save, Cancel and work that starts immediately

Treat **Save** as applying several settings stores, not as one all-or-nothing transaction. Most ordinary preference edits are committed on Save; language and appearance are previews that can be rolled back until their commit succeeds. Save validates shortcut bindings before writing. It then performs fallible writes for the token, AnimeJaNai configuration when changed, Native preferences and runtime choice, and system integrations. The remaining appearance, language preference, feeds, HDR preferences and shortcuts are committed after those writes succeed.

If a later save step fails, earlier independent writes that already succeeded remain saved. For example, if an integration fails after the token was written, cancelling afterward does not undo the token write. The dialog explains that other settings may already be saved; Cancel restores unsaved language and appearance previews, not previously completed writes. System-integration changes have their own rollback attempt, but an OS error can prevent complete rollback.

Some page actions are operations, not pending preferences:

- **Test** makes an API request using the token currently in the field.
- **Components** downloads, verification and local-runtime installation run from that page's own controls. Its **Cancel** stops the active component operation; closing Settings does not act as that button.
- **Benchmark → Run benchmark** starts a measurement using the current selections; it is not saved by the Settings footer.

## Settings identity, files and logs

The application keeps the existing Qt settings identity: organization **Mikai**, application **Mikai Wrapper**. Updates preserve preferences stored under that identity; do not delete or rename the settings store to troubleshoot an unrelated issue.

The API token is stored in `.env` as described above. The app session log is `mikai.log`: it is written beside the executable when that directory is writable, otherwise under Qt's application-local-data directory (including read-only AppImage cases). At startup, an existing log at or above 2 MiB is rotated to `mikai.log.1` before the new session is appended. Crash reports are written beside the log. There is no in-app log-location setting.

Logs and benchmark reports can contain diagnostic details and environment or file-path information. Review them before sharing. Do not share `.env` or expose its token. For where benchmark reports and supplied clips live, see the [processing guide](processing-guide.md#benchmarking).
