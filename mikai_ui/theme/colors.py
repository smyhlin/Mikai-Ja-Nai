"""Central color palette for the desktop client."""

BACKGROUND = "#0b0b0f"
SURFACE = "#16161d"
SURFACE_HOVER = "#22222b"
SURFACE_ACTIVE = "#2b2b35"
ACCENT = "#bdacff"
ACCENT_HOVER = "#cfc4ff"
DANGER = "#ff6b78"
TEXT = "#e7e7ea"
MUTED = "#a0a0ab"
BORDER = "#30303a"
VIDEO_BACKGROUND = "#000000"

# Player chrome sits on black video, so a translucent fill collapses to the
# video color. The bar is an opaque raised band instead, cut off by a hairline.
CHROME_FILL = "#121218"
CHROME_HAIRLINE = "rgba(231, 231, 234, 0.09)"

# Icon buttons are borderless and only show a fill under the pointer, so the row
# reads as glyphs over video rather than a row of grey boxes.
GHOST_HOVER = "rgba(231, 231, 234, 0.08)"
GHOST_PRESSED = "rgba(231, 231, 234, 0.14)"

# The pill and the speed popover float over live video, so a translucent dark
# fill keeps the picture readable underneath while still reading as chrome.
CHROME_PILL_FILL = "rgba(11, 11, 15, 0.72)"
CHROME_FLYOUT_FILL = "rgba(11, 11, 15, 0.88)"

# Qt greys out a disabled button's text but not its icon, which leaves a
# transport control looking live while it is inert.
ICON_DISABLED = "rgba(160, 160, 171, 0.45)"
