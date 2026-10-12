# Qt 6.12 migration — validation report

Every gate below was run on this branch against
`C:/temp/qt6/6.12.0/mingw_64`, MinGW-w64 w64devkit GCC 16.2, CMake 4.4.3.
Results are reported as observed; the raw commands are in `release-evidence.md`.

## Gates

| # | Gate | Result |
|---|---|---|
| 1 | Configure (`cmake --preset release`) | PASS — configures against the 6.12 prefix in 12.1 s |
| 2 | Build `mikai` + `mikai_tests` | PASS — rc=0, **0 errors, 0 warnings** |
| 3 | Build all 34 probe targets | PASS — rc=0, **0 errors, 0 warnings** |
| 4 | Deprecation policy | PASS — whole tree (app, tests, probes, `native/`) compiles with `QT_DISABLE_DEPRECATED_UP_TO=0x060C00` **and** `QT_WARN_DEPRECATED_UP_TO=0x060D00`; nothing Qt 6.12 deprecated is used, and nothing Qt 6.13 will deprecate is either |
| 5 | `ctest --test-dir build/release` | PASS — **4/4**: `NativeStageContracts`, `LauncherContracts`, `DistContracts`, `mikai_tests` |
| 6 | Per-suite detail (isolated launcher) | PASS — 35 suites PASS, `TestLiveOracle` SKIP (no API key), launcher exit 0 |
| 7 | `widget_pixel_probe` | PASS — 25/25 checks ok, 0 failed, 0 skipped, 1 instrumentation unavailable (on-screen readback: no display available) |
| 8 | `shell_probe` | PASS — 10/10 ok, 0 failed, 0 skipped |
| 9 | `dialog_probe` | PASS — 20/20 ok, 0 failed, 0 skipped |
| 10 | `anime4k_menu_probe` | PASS — 132/132 ok, 0 failures |
| 11 | `engine_popup_probe` | PASS — 93/93 ok, 0 failed, 0 skipped |
| 12 | `compare_probe` | PASS — 343/343 ok, 0 failures |
| 13 | `interaction_probe` | PASS — 180/180 ok, 0 failures |
| 14 | Translation toolchain | PASS — 6.12 `lupdate` finds 1016 source texts (0 new, 1016 existing) and the catalogue is **byte-unchanged** after an in-place update; `lrelease` 1016 finished / 0 unfinished |
| 15 | Deployment (`windeployqt --release --translations uk`) | PASS — deploys `Qt6Core/Gui/Widgets/Network/OpenGL/OpenGLWidgets/Svg` at 6.12.0.0, the MinGW runtime byte-identical to the Qt prefix's, and a plugin set that is the 6.8.3 set plus four image formats |
| 16 | Plugin discovery | PASS — the app starts with **no** `QT_PLUGIN_PATH` in the deployed layout (the 6.8.3 development tree needed it) |
| 17 | Application run | PASS — real window (`Qt6120QWindowIcon`, title "Mikai"), `mikai.log` reaches `[INFO] mikai: player runtime standard stock-20c217472c01` 0.76 s after `---- mikai started ----`, no `mikai-crash-*.txt` |
| 18 | `TestFeedRender` (new) | PASS — 5/5. The home feed renders 6/6 canned cards and shows no carousel retry; a title page renders 12/12 episode pills with distinct numbers; and a backend that answers 522 produces a visible failure on **both** pages — the carousel's retry appears and the title page's status line moves off its loading text |
| 19 | `TestFeedRender` dead-origin routes | PASS — every route the two pages use (`ongoing`, `popular`, `updates`, `schedule`, `detail`, `player`) comes back from a 522 origin as `http_error` **with status 522**, and the code chip visible on each page reads `522` |
| 20 | Live application run (real API, real window) | PASS — `build/release/mikai.exe` started, resolved its runtime in 0.6 s and rendered a fully populated Ukrainian feed against the live API: "Нещодавно відкриті" (Бліч, Людина-бензопила), "Зараз виходить" with episode progress, "Популярне на Mikai" (Ван Піс, Магічна битва), "Останні оновлення" and "Графік виходу". Inspected screenshot `.probe-out/qt612-live-app.png`; no new crash report |

Gate 20 is the one gate no suite can replace here: `TestLiveOracle` is keyless on this
machine and would skip, so the live path is proven by observation — a real process,
a real window and an inspected screenshot — rather than by a fixture. The API was
verified reachable first (`/meta` and `/anime` both 200), and the log records each
run reaching its runtime line with no crash report.

### The gap this suite closes

Every gate above except #18 tests a boundary in isolation or a probe that talks
to the live API. None of them rendered the home feed or a title page, so "the app
runs" was the strongest claim the old suite supported — and a page that rendered
nothing, or sat on its loading text, passed all of it. That is what a user saw
during the 2026-10-12 outage, and it was indistinguishable from a broken client
from the inside.

`TestFeedRender` renders both pages against canned payloads served by a loopback
`QTcpServer` (no outbound request at all) and asserts the two directions: content
appears when the backend answers, and a *named* failure appears when it does not.
The retry is found by its label rather than its object name, because the updates
and schedule rows carry an always-visible "Refresh" action under the same
`textAction` name — an object-name query would count a button that is supposed to
be on screen.

### Why a failure says what it says

A failure reaching the UI used to be flattened to a sentence at the `Runner`
boundary, so a Cloudflare 522, a rate limit and a cancelled request could all
read the same. `backend::Failure` now carries the stable code and the HTTP
status beside the message, and `FailureView` renders them: the backend's line,
the machine token (`HTTP 522`, `rate_limited · HTTP 429`, `network_error`) and
a retry. The token is deliberately untranslated — it is the part of the line
that has to survive a screenshot.

`apiErrorFromHttp` also dropped the status: a non-envelope body — a proxy page,
a gateway error — took the transport branch and reported `network_error`
with status 0, losing the one fact that names it. That is what a Cloudflare 522
looked like from inside the app, and gate 19 pins it.

Deployed file set, 6.12 vs the installed 6.8.3 dist: identical for every DLL and
plugin directory, with `imageformats/` gaining `qicns`, `qtga`, `qtiff`, `qwbmp`
(the `qtimageformats` addon). Nothing the 6.8.3 dist had is missing.

## Disposition of the ledger's open rows

`impact-ledger.csv` records what the audit found; its `status` column is the
audit's own verdict, not the migration's. Disposition of every `open` row:

| Row | Audit verdict | Disposition |
|---|---|---|
| Q6.12-H01 QSaveFile permissions | required-qualification | **Not present**: `grep -rn setPermissions src/` → 0 hits; `writeEnvFile` (MikaiClient.cpp:322) only sets `setDirectWriteFallback(false)` before `open()` and commits. Nothing to move before `commit()`. |
| Q6.12-H02 negative `QTimer` interval | audit | **Not reachable**: the only computed interval is `1000.0 / std::max(1, flicker_->value())` (CompareWindow.cpp:1132) and the spin box is clamped to 1–30. No change. |
| Q6.12-H04 `QDirIterator` (13 sites) | recommended-modernization | **Deferred** — still supported in 6.12 (the tree compiles with the 6.12 cutoff), and `QDirListing` is a semantics rewrite, not a rename. |
| Q6.12-H06 `QVariant::userType()` | required-compatibility | **Not a compatibility item**: `Qt6.12.0/include/QtCore/qvariant.h:572` declares `int userType() const { return typeId(); }` with no deprecation marker; it compiles under the 6.12 cutoff with no warning. The deprecation at `:670` is `QVariant::Type`. Migrating to `typeId()` is cosmetic and deferred. |
| Q6.12-H15 string-based `invokeMethod` (2 sites, one probe) | required-qualification | **Deferred** — still supported; probe-only. |
| Q6.12-H20 `QPair`/`qMakePair` (42 references) | recommended-modernization | **Deferred** — a strict-mode restriction in 6.12, not a deprecation; the tree compiles under the cutoff. |
| Q6.12-H37 `windeployqt` deployment surface | required-qualification | **Verified** — gate 15 above; the hand-invocation is kept by design (`cmake/dist.cmake` owns rules `qt_generate_deploy_app_script` does not model) and `--release --translations uk` are current flags. |

Everything else in the ledger is `not-present`, and the strongest evidence for
that is gate 4: the whole tree compiles with deprecated-up-to-6.12 APIs removed
from the headers.

## Not verified (stated, not implied)

* **The live API path** — while this work was done, `api.mikai.me` and
  `mikai.me` answered Cloudflare **522** (origin unreachable) to every request,
  from `curl` as well as from the app, while other hosts answered 200. The live
  oracle (`MIKAI_LIVE=1`) and `title_page_probe` therefore could not be
  exercised, and no claim is made about the network path here. What the outage
  did establish is the client side: the app's named per-block errors and retries
  were the correct rendering of a dead backend, and `TestFeedRender` now proves
  the same code renders content when a backend answers.

* **Clean-machine installation** — no VM without a Qt SDK was available; the
  deployment gate covers the deployed set and its versions, not a fresh OS.
* **Pixel comparison against the 6.8.3 build** — the pixel probes pass their own
  requirements on 6.12, but no side-by-side image diff of 6.8.3 vs 6.12 output
  was taken.
* **Performance beyond startup** — only the start-to-runtime-line time is
  recorded (0.76 s here); no frame-time or memory comparison was made.
* **`dist` target run** — deliberately not executed into `dist-mikai/`: it
  refuses a populated output by design, and that folder holds the installed
  application and its per-machine native assets.
* **`MIKAI_WITH_MODEL_INSPECTION=ON` / `native` preset** — not configured (it
  needs `ORT_INCLUDE_DIR`); the `native/` sources do compile in the default
  preset and carry the same deprecation level.
* **Qt 6.12.1** — not released at the time of writing; the pin is `6.12`, so a
  patch update needs no source change (procedure in `rollout-and-rollback.md`).
