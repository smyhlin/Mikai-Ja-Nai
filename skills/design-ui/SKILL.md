---
name: design-ui
description: >
  Design and build polished, non-generic UI with OS-native first: PySide6
  (Qt Widgets/QML) and Slint (Rust). React + Tailwind v4 + shadcn/Radix only
  as web fallback. Use whenever you create or restyle any interface surface —
  desktop windows, dialogs, settings, dashboards, forms, nav, and game overlays
  (start screens, HUD, menus). Covers design tokens, layout, typography, color,
  spacing, motion, and the anti-"AI-slop" rules that keep output from looking
  generic. Triggers on "design", "UI", "make it look good", "polish", "theme",
  "style", "redesign", "ugly", "clean up", "pyside", "slint", "native".
---

# Design & UI (OS-native first)

Make interfaces that look intentional and premium, not template-generic.
Default target is **OS-native**: PySide6 or Slint. Use the React + Tailwind +
shadcn stack only when the task explicitly asks for web.

**Read `references/` for depth** (loaded on demand — don't inline it all):
- `references/refined-ui.md` — full product-chrome/overlay design system (token
  values transfer 1:1 to native; CSS snippets are illustrative).
- `references/typography.md` — type scale, pairing, rhythm.
- `references/surfaces.md` — elevation, borders, shadows, layering.
- `references/animations.md` — motion, easing, transitions.
- `references/performance.md` — keep UI smooth (60fps, no jank).

## 0. Target selection (do this first)

- **PySide6** (Python): `QWidget`/QML + QSS. Theme lives in one `theme.py`
  (constants + `build_stylesheet()`), applied once via `app.setStyleSheet()`.
  Layouts: `QVBoxLayout` / `QHBoxLayout` / `QGridLayout` / `QStackedLayout`.
  No absolute positioning.
- **Slint** (Rust): `.slint` components, tokens in one shared `global Theme`,
  standard layouts (`VerticalBox`, `HorizontalBox`, `GridBox`). No absolute
  positioning except intentional overlay layers.
- **Web fallback** (only on explicit ask): React + Tailwind v4 CSS-first
  `@theme` + shadcn/ui in `src/components/ui`.

## 1. Design-system-first (do this before styling anything)

Define the system once, then compose from it. **Never** sprinkle ad-hoc values.

- **Slint tokens** — one shared global, imported everywhere:
  ```slint
  global Theme {
      in property <color> bg: #0b0b0f;
      in property <color> surface: #16161d;
      in property <color> fg: #e7e7ea;
      in property <color> muted: #a0a0ab;
      in property <color> primary: #14b8a6;
      in property <color> border: #26262f;
      in property <length> radius: 12px;
  }
  ```
- **PySide6 tokens** — one `theme.py`, QSS built from it (single source of truth):
  ```python
  BG, SURFACE, FG, MUTED, PRIMARY, BORDER = (
      "#0b0b0f", "#16161d", "#e7e7ea", "#a0a0ab", "#14b8a6", "#26262f")
  RADIUS = 12
  def build_stylesheet() -> str:
      return f"QMainWindow {{ background: {BG}; }} QPushButton {{ border-radius: {RADIUS}px; }}"
  ```
- **Web tokens** (fallback only) — Tailwind v4 CSS-first in `src/styles.css`
  under `@theme`; consume as utilities. Plus the v4 base fix so buttons show
  a pointer (`button:not(:disabled), [role="button"] { cursor: pointer; }`
  in `@layer base`). Native targets skip this — Qt/Slint use platform cursors
  (`Qt.PointingHandCursor` only on interactive controls).
- **Ban ad-hoc styling:** no raw hex in `.py` / `.slint` / JSX outside the
  theme file, no `text-white`-style literals, no magic numbers (`14px` here,
  `27px` there). A new value becomes a token or a scale step.

## 2. Components — native controls first

- **PySide6:** platform widgets (`QPushButton`, `QDialog`, `QMenu`,
  `QTabWidget`, `QLineEdit`) styled via the theme stylesheet. Own the set
  under `ui/` with variant helpers mirroring shadcn roles (primary /
  secondary / destructive).
- **Slint:** standard components (`Button`, `LineEdit`, `ComboBox`,
  `TabWidget`) restyled with `Theme`. Same variant roles.
- **Web (fallback):** shadcn/ui components (Radix + `cva` + `tailwind-merge`),
  generated into `src/components/ui`, styled via tokens.
- Match the existing surface when editing in place; don't introduce a second
  visual language.

## 3. The quantified rubric (cheap rules that prevent "ugly")

- **≤ 3–5 colors total** (one primary + neutrals + at most one accent). No random
  extra hues. Don't default to purple unless asked.
- **≤ 2 font families** (often one — the platform UI font). Pair a
  display/heading with a body, or use one.
- **Line-height 1.4–1.6** for body; tighter for large headings.
- **When you override a background color, override the foreground/text color too**
  (contrast must hold — check both light and dark; verify against
  `QPalette` or Slint dark/light `Theme` variants).
- **Min-window-first**: design at the smallest supported window (desktop) or
  ~390px width (web/mobile) first, then scale up. No clipped controls, no
  horizontal scroll; hit targets ≥ 24px desktop, ≥ 44px touch.
- **Consistent spacing scale** (4/8-based). Generous whitespace beats cramming.
- **One accent, used sparingly** for primary actions — not everywhere.

## 4. Anti-AI-slop (the tells that make output look generic — avoid)

- **No gradient-blob filler**, no giant hero gradients as a substitute for content.
- **No emoji as icons** — native: Lucide SVGs via `QIcon` / Slint `@image-url`;
  web: `lucide-react`.
- **No hand-drawn SVG** illustrations/maps/charts — native: `pyqtgraph` (PySide6)
  or a real chart view; web: `recharts`. Or real generated images.
- **No placeholder images / lorem-gray boxes** in the final product — real content;
  HiDPI-aware (`devicePixelRatio` / `@image-url` scales).
- **Avoid the overused-font look** (default system-only with no hierarchy, or
  novelty picks). Platform font + deliberate weights.
- **Every element earns its place.** Cut decorative noise. Establish a system,
  then vary with intent — not randomness.

## 5. Layout & hierarchy

- Clear visual hierarchy: one primary action per view; size/weight/color express
  importance. See `references/typography.md` and `references/surfaces.md`.
- Real layout structure (native layout boxes, grid/flex on web; container
  max-widths), not absolute-position hacks. Align to a consistent grid.
- Concentric radii: outer = inner + padding; step down one token per nesting
  level. Pills (`9999px`) only for small chips — not large cards.
- Empty states, loading states, and error states are part of the design — don't
  ship blank/janky intermediate states.

## 6. Motion (subtle, purposeful)

- Short, eased transitions (150–250ms) on hover/press/enter; enumerate animated
  properties, never "all"; prefer opacity/position over layout.
- Native: `QPropertyAnimation` (PySide6) / `animate` blocks (Slint). Respect the
  OS reduced-motion setting, same as `prefers-reduced-motion` on web.
  ```python
  anim = QPropertyAnimation(widget, b"opacity")
  anim.setDuration(150)
  anim.setStartValue(0.0)
  anim.setEndValue(1.0)
  anim.start()
  ```
  ```slint
  animate opacity, x { duration: 150ms; easing: ease-out; }
  ```
- `references/animations.md` / `performance.md` principles transfer as-is;
  translate their CSS/Motion/Tailwind snippets to the native equivalents above.

## 7. Game overlays (native canvas + overlay)

The gameplay canvas is a native widget (e.g. `QOpenGLWidget` / Slint custom
canvas). Style the **overlay layer**: start screen, HUD, score, menus, pause,
touch controls. Stack via `QStackedLayout` / Slint overlay layer, keep it
readable over the canvas (translucent backdrop, contrast), and keep it out of
the gameplay input/focus path.

---

## Finish checklist (before you call UI done)
- Tokens defined once (Slint `global Theme` / `theme.py` / `@theme`); no ad-hoc
  hex / magic numbers in views.
- ≤ 5 colors, ≤ 2 fonts, consistent spacing scale, concentric radii.
- Contrast holds; foreground overridden wherever background is (both themes).
- Min window has no clipped controls; targets ≥ 24px desktop / 44px touch.
- Real icons/images/charts — none of the anti-slop tells.
- Loading/empty/error states handled; motion subtle and reduced-motion-safe.
- Rendered and eyeballed in a real native window on the target OS, not just
  fetched as HTML.
