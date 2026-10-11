# Qt 6.12 migration — implementation plan and decisions

Target: **Qt 6.12.0 LTS**, MinGW-w64, Windows x64. Baseline: commit `6bfa80f` on
`main` (Qt 6.8.3). Branch: `migrate-to-qt-6.12` (the requested name `migrate to
qt  6.12` is not a legal git branch name — git rejects spaces — so it is spelled
with dashes).

**Scope decision (owner instruction, in-session): target-only.** No dual-version
support, no compatibility shims, no `QT_VERSION_CHECK` branches: the tree builds
against 6.12 and the 6.8.3 pin is removed everywhere. The 6.8.3 SDK is left on
disk untouched but nothing in the repository references it.

## Phase 1 — Inventory and baseline

`docs/qt612/inventory.md` (build/toolchain/Qt surface/deployment/persisted data)
and `docs/qt612/impact-ledger.csv` (40 hazards from the Qt 6.9–6.12 changes,
each with `file:line` evidence and a verdict). Baseline evidence is in
`release-evidence.md`.

## Phase 2 — Obtain and qualify Qt 6.12

The SDK is the **official Qt MinGW package set**, fetched from the public
repository and unpacked with Windows' own `bsdtar` — no Qt account, no
`aqtinstall`, no Python:

| Package | Why it is needed |
|---|---|
| `qtbase` | Core, Gui, Widgets, Network, OpenGL, OpenGLWidgets, Test |
| `qttools` | `lupdate`, `lrelease`, `windeployqt`, `designer` (LinguistTools) |
| `qtsvg` | `Qt6Svg` — the SVG image-format plugin the dist deploys |
| `qtimageformats` | `qwebp` and the other image formats (a hard runtime dependency, see `readme.md`) |
| `qtdeclarative` | `Qt6Qml.dll` — **`lupdate` imports it since Qt 6.11** and exits 53 without it |
| `qtlanguageserver` | the language-server module `qttools` links against |
| `qttranslations` | Qt's own translated catalogues, as in the 6.8.3 prefix |
| `opengl32sw`, `d3dcompiler_47` | the software GL fallback and D3D compiler the dist deploys |
| MinGW runtime archive | `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll` beside the Qt DLLs |

Two of those were **not** optional and were found by running the tools, not by
reading a manifest: `lupdate` fails silently (exit 53, no output) without
`Qt6Qml.dll`, and `windeployqt` has no WebP plugin to deploy without
`qtimageformats`.

Toolchain: the tree keeps **w64devkit GCC 16.2** while Qt 6.12 is built with
**GCC 15.1.0** (`mkspecs/qconfig.pri`). That is the same mixed-runtime structure
the tree already had (Qt 6.8.3 was GCC 13.1.0) and it is proven by the build,
the suite and a real app run, not assumed.

## Phase 3 — Build compatibility

`CMakePresets.json` points at `C:/temp/qt6/6.12.0/mingw_64`;
`CMakeLists.txt` pins `find_package(Qt6 6.12 REQUIRED …)`. No other build-system
change was made: the target graph, the preset set and `cmake/dist.cmake` are
untouched.

Exactly **one** source fix was required: `QDragEnterEvent(const QPoint&, …)` in
`src/probes/player/EnginePopupProbe.cpp` is an unversioned deprecation
(`QT_DEPRECATED_VERSION_X_6_16`) that warns regardless of the cutoff; it now
uses the `QPointF` constructor, matching the adjacent `QDropEvent`.

## Phase 4 — Deprecation policy

`CMakeLists.txt` compiles the whole tree — app, tests, probes and `native/` —
with:

* `QT_DISABLE_DEPRECATED_UP_TO=0x060C00` — a use of anything Qt 6.12 deprecated
  is a **build error**. This is stronger than the staged warn-then-disable
  sequence the migration playbook suggests, and it is affordable because the
  tree was already clean: the whole tree compiles under it with zero warnings.
* `QT_WARN_DEPRECATED_UP_TO=0x060D00` — Qt 6.13's deprecations, which 6.12's
  headers already annotate, are reported now instead of after the next bump.

## Deliberately deferred (documented, not silently skipped)

| Item | Why deferred |
|---|---|
| `QDirIterator` → `QDirListing` (13 sites) | deprecated, not removed in 6.12; the migration is a semantics rewrite (flags, symlinks, hidden entries) and belongs in its own change |
| `QPair`/`qMakePair` → `std::pair` (42 references) | strict-mode-only restriction in 6.12, still fully supported |
| `QVariant::userType()` → `typeId()` (2 sites in `PlayerDialog`) | still available in 6.12; a two-line mechanical change for a later modernization pass |
| string-based `QMetaObject::invokeMethod` (2 sites in a probe) | still available; probe-only |
| `QTimer` → `QChronoTimer`, `QFuture::cancelChain`, `QRangeModel`, `QSortFilterProxyModel::dataChangeRelevanceFlags` | adoption requires a demonstrated benefit and tests; the tree uses its own `Runner` and `QTableWidget`, not models |
| `qt_standard_project_setup(REQUIRES 6.12)` | would opt into QTP policies (resource prefixes, global AUTOUIC/AUTORCC) with no benefit here: the tree sets AUTOMOC per target and its own resource handling |
| `qt_generate_deploy_app_script` instead of `windeployqt` | `cmake/dist.cmake` owns the deployment rules and their contract tests (`DistContracts`); replacing the mechanism is a separate change |
| `QT_ENABLE_STRICT_MODE_UP_TO` | a separate qualification job, not a compatibility requirement |
| Qt Quick/QML, Charts→Graphs, WebEngine/PDF, Multimedia, Canvas Painter, StyleKit, hot reload | the tree uses none of these modules; there is nothing to migrate |

## Phase 5 — Verification

Every gate, with its observed result, is in `validation-report.md`; the raw
commands and output are in `release-evidence.md`.
