# Anime4K shaders

The Anime4K GLSL files are third-party content and are **not** stored in this
repository. What is stored is the pin:

- `anime4k-manifest.json` names one commit of
  [`bloc97/Anime4K`](https://github.com/bloc97/Anime4K) and the SHA-256 of every
  one of its 49 `glsl/**.glsl` files.
- `cmake/FetchAnime4K.cmake` materialises that pin into a directory, verified file
  by file, and can move the pin. It runs through `cmake -P`, so the build, the
  CTest check and a maintainer use the same code; no interpreter beyond CMake is
  involved and nothing Python-related ships or is required.

CMake calls the script at configure time (`cmake/FindOrFetchAnime4K.cmake`), so a
checkout builds the same shaders the manifest describes without carrying a copy
of them:

```bash
cmake -S . -B build                     # downloads one tarball of the pinned commit
cmake -S . -B build -DMIKAI_ANIME4K_ARCHIVE=/path/Anime4K-<commit>.tar.gz   # offline
```

- The archive is cached in `<build>/_dist-cache/anime4k/`; a configure whose pin
  is already staged in `<build>/shaders` reuses it without any network access.
- `MIKAI_ANIME4K_SHADERS_DIR` overrides where the files are staged,
  `MIKAI_ANIME4K_CACHE_DIR` where the tarball is kept.
- Every staged file is digest-checked against the manifest before it is written;
  a mismatch fails the configure instead of shipping unverified shaders.
- `mikai_anime4k_shaders` (CTest) re-checks the staged tree, so a locally edited
  or missing shader fails the suite.

## Moving the pin

```bash
# what the suite runs
cmake -DMODE=check -DMANIFEST=packaging/shaders/anime4k-manifest.json \
      -DSHADERS_DIR=build/shaders -P cmake/FetchAnime4K.cmake

# move the pin: rewrites the manifest and stages the new files
cmake -DMODE=pin -DCOMMIT=<40-hex sha> \
      -DMANIFEST=packaging/shaders/anime4k-manifest.json \
      -DSHADERS_DIR=build/shaders -DCACHE_DIR=build/_dist-cache/anime4k \
      -P cmake/FetchAnime4K.cmake
```

`MODE=pin` lists `glsl/**.glsl` through the GitHub tree API, downloads each blob,
writes the manifest and stages the files. A file that the new commit no longer
publishes is reported by `MODE=check` instead of being deleted, because a preset
chain in `src/player/shaders/Anime4K.cpp` may still name it.

After a pin bump: rebuild, run `mikai_tests` (`TestAnime4K` pins every chain in
order), refresh translations if labels changed, and check that every preset still
has a complete chain.

## Why the chains are file lists, not merged shaders

mpv takes `glsl-shaders` as a list and compiles every file's hooks into one pass
graph, so concatenating a preset's files into a single `.glsl` changes nothing:
the hook phases (`LUMA`, `CHROMA`, `CHROMA_SCALED`, `NATIVE`, `PREKERNEL`,
`MAIN`) and their order are identical either way. Merging is required only where
a single-shader API consumes the chain — video2x hands FFmpeg's `libplacebo`
filter one `custom_shader_path` — and would cost us the per-shader inventory,
the per-file licence headers and the ability to compose a preset from stages.

## Chain order notes

- `AutoDownscalePre_x2/x4` sit immediately after the first upscale and before the
  second restore (upstream: x4 "should be placed between two Upscale shaders", x2
  "after the first Upscale shader"). Moving them after the second restore makes
  that CNN run on the 2x image and its result is then thrown away by the
  downscale.
- `hq_gan` is upstream v4.1's GAN mode, `Restore_GAN_UUL -> Upscale_GAN_x4_UUL ->
  Restore_CNN_Soft_M -> Upscale_CNN_x2_M`, with no clamp or downscale stage. It is
  the heaviest preset in the catalogue and is exposed under Maximum, not as a
  default.
