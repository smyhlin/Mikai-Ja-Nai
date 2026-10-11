# Qt 6.12 migration — rollout and rollback

## What a rollback is

The migration is a **source + toolchain** change, so rolling back is a git
operation plus a Qt prefix switch — nothing about the shipped product's state
changes:

| Artifact | Where it stands |
|---|---|
| Baseline source | commit `6bfa80f` on `main` (Qt 6.8.3, 4/4 ctest PASS) |
| Migration source | branch `migrate-to-qt-6.12` |
| Previous Qt SDK | still installed at `C:/temp/qt6/6.8.3/mingw_64`, untouched |
| Previous packaged app | `dist-mikai/mikai.exe` and its Qt 6.8.3 runtime, untouched (byte-identical, see below) |
| Per-machine native assets | `dist-mikai/native/**` and `build/release/native/**` — never rewritten by this migration |

Rollback procedure:

```sh
git switch main                    # source back to Qt 6.8.3
# and, if the build tree is reused:
rm -f build/release/CMakeCache.txt && rm -rf build/release/CMakeFiles
C:/temp/w64dev/w64devkit/bin/cmake.exe --preset release   # preset then points at 6.8.3
```

`build/release/native/` (the real development native store), `_dist-cache/` (the
pinned mpv archive) and `_test-runs/` are not touched by a reconfigure, so the
rollback does not re-download or re-install anything per-machine.

## Rollout sequence for this change

1. **Qualification build** — done on this branch: 0 errors, 0 warnings,
   `ctest` 4/4, GUI probe and app run (see `validation-report.md`).
2. **Dist assembly** — `cmake --build build/release --target dist` refuses a
   populated output directory by design. The currently installed
   `dist-mikai/` was therefore left alone; a fresh folder is assembled by
   pointing the target at an empty output (or by deleting a *previously
   assembled* one). The deployed Qt runtime changes from 6.8.3 to 6.12.0 in
   that step, together with its plugins.
3. **UI-only update of an existing installation** — copy the verified
   `mikai.exe` into the installed folder, but note that a Qt **minor** bump also
   changes `Qt6*.dll` and the plugin set beside it; a Qt-version change is a full
   re-deploy, not an exe swap.
4. **Clean-machine check** — the packaged folder must run with no Qt SDK on
   `PATH` and no `QT_PLUGIN_PATH` set; the plugins are found relative to the exe.
   (`build/release` needs `QT_PLUGIN_PATH` only because it is a development
   tree, exactly as it did on 6.8.3.)
5. **Watch** — startup line in `mikai.log` (`[INFO] mikai: player runtime …`)
   and the absence of `mikai-crash-*.txt` beside the exe.

## What must not be done

* Do not copy the Qt 6.8.3 `Qt6*.dll` or plugin folders into the new package:
  Qt rejects a plugin built against a higher minor, and a mixed set is the one
  failure mode that looks like a random crash rather than an error.
* Do not delete `C:/temp/qt6/6.8.3/mingw_64` until the new package has been
  accepted — it is the rollback path for the *toolchain*, and the installed
  `dist-mikai/` still carries its runtime.
* Do not point `MIKAI_MINGW_RUNTIME_DIR` at w64devkit's runtime: `CMakeLists.txt`
  refuses it by digest on purpose, because a different MinGW generation beside
  the exe would replace the thread runtime Qt was built against.

## Version pinning for the next patch

Qt 6.12.1 is planned for 2026-11-12 and was not released when this migration
was made. When it lands, the same procedure applies: fetch the corresponding
`6.12.1-*` archives for the same package list, unpack over
`C:/temp/qt6/6.12.1/mingw_64`, update `CMakePresets.json`, reconfigure and
re-run the gates in `validation-report.md`. `find_package(Qt6 6.12 …)` accepts
any 6.12.x, so no source change is expected.
