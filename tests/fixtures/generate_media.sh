#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# M02 hermetic fixture media generator (Linux port).
#
# Every asset is produced from ffmpeg's own lavfi sources, so the matrix is
# project-owned and licence-clean: no third-party footage is downloaded,
# copied or re-encoded. Re-running this script reproduces the same inputs
# (lavfi is deterministic; only the muxer timestamp container differs).
#
# This is the Linux/bash port of generate_media.cmd: the same clips, under
# the same filenames, with the same encoder parameters, so both generators
# stay interchangeable for the clips they share. Blocks 12 and 13 add the
# two HDR regression clips from plan Phase 0.3 (T-107); they currently have
# no .cmd counterpart and are produced by this generator only.
#
# Usage:
#     FFMPEG=/path/to/ffmpeg tests/fixtures/generate_media.sh
#
# FFMPEG defaults to "ffmpeg" on PATH. MEDIA_DIR and HLS_DIR override the
# output roots (default: this script's own media/ and hls/); the nightly
# soak lane regenerates the fixtures at those default roots inside its own
# fresh checkout. No Python, no Conda, no network.
#
# Every block is skippable with a clear message when the toolchain is
# missing: a nonexistent ffmpeg, or an ffmpeg build without the block's
# encoder, prints a SKIP line and the matrix continues. A summary at the
# end lists every expected output, so a caller can decide - and acceptance
# jobs fail when the file they need is absent rather than substituting a
# synthetic lavfi source.
# ---------------------------------------------------------------------------

FFMPEG="${FFMPEG:-ffmpeg}"
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MEDIA="${MEDIA_DIR:-$HERE/media}"
HLS="${HLS_DIR:-$HERE/hls}"

# An empty ENCODERS string makes every have_encoder check fail, which is the
# correct verdict for a broken ffmpeg: skip loudly rather than guess.
if command -v "$FFMPEG" >/dev/null 2>&1; then
    echo "using ffmpeg: $FFMPEG"
    "$FFMPEG" -hide_banner -version 2>/dev/null | grep -m1 'ffmpeg version' || true
    ENCODERS="$("$FFMPEG" -hide_banner -encoders 2>/dev/null)"
else
    echo "SKIP: ffmpeg not found (FFMPEG=$FFMPEG); no fixture can be generated." >&2
    ENCODERS=''
fi

have_encoder() {
    [ -n "$ENCODERS" ] && printf '%s\n' "$ENCODERS" | grep -qw "$1"
}

if ! have_encoder libx265; then
    echo "SKIP: this ffmpeg has no libx265 encoder; every HEVC clip below is skipped." >&2
fi
if ! have_encoder libx264; then
    echo "SKIP: this ffmpeg has no libx264 encoder; the H.264 HLS rungs below are skipped." >&2
fi

mkdir -p "$MEDIA" "$HLS/v180" "$HLS/v360" "$HLS/v720"

SRC8='yuv420p'
SRC10='yuv420p10le'
TAGS='-tag:v hvc1'
X265='-x265-params log-level=error:pools=2'

# --- source rationale ------------------------------------------------------
# testsrc2 is used wherever "real motion, real chroma" is required: it has
# genuine inter-frame motion plus saturated bars. A still frame would make
# every later interpolation/temporal gate vacuous.
# ===========================================================================
# 1. The primary 320x180 clip: 24 fps, 8-bit HEVC, 12 s. The geometry and
#    duration external_media_probe requires (320x180, >10 s, colourful).
# ===========================================================================
if have_encoder libx265; then
    "$FFMPEG" -hide_banner -y -f lavfi -i 'testsrc2=s=320x180:r=24:d=12' \
      -an -c:v libx265 -preset faster -crf 26 -pix_fmt "$SRC8" $TAGS \
      -movflags +faststart "$MEDIA/motion_320x180_24fps_8bit_hevc.mp4" \
      || echo "FAILED: motion_320x180_24fps_8bit_hevc.mp4" >&2
else
    echo "SKIP 1: motion_320x180_24fps_8bit_hevc.mp4 (no libx265)" >&2
fi

# 2. Same geometry and duration at 24000/1001 fps (23.976).
if have_encoder libx265; then
    "$FFMPEG" -hide_banner -y -f lavfi -i 'testsrc2=s=320x180:r=24000/1001:d=12' \
      -an -c:v libx265 -preset faster -crf 26 -pix_fmt "$SRC8" $TAGS \
      -movflags +faststart "$MEDIA/motion_320x180_23976fps_8bit_hevc.mp4" \
      || echo "FAILED: motion_320x180_23976fps_8bit_hevc.mp4" >&2
else
    echo "SKIP 2: motion_320x180_23976fps_8bit_hevc.mp4 (no libx265)" >&2
fi

# 3. Same geometry at 10-bit HEVC (Main10), so bit-depth switching is real.
if have_encoder libx265; then
    "$FFMPEG" -hide_banner -y -f lavfi -i 'testsrc2=s=320x180:r=24:d=12' \
      -an -c:v libx265 -preset faster -crf 26 -pix_fmt "$SRC10" $X265 \
      -profile:v main10 $TAGS -movflags +faststart \
      "$MEDIA/motion_320x180_24fps_10bit_hevc.mp4" \
      || echo "FAILED: motion_320x180_24fps_10bit_hevc.mp4" >&2
else
    echo "SKIP 3: motion_320x180_24fps_10bit_hevc.mp4 (no libx265)" >&2
fi

# ===========================================================================
# 4-6. 1920x1080 at 24 fps 8-bit, 24 fps 10-bit, and 24000/1001 8-bit.
#      6 s each: enough for a seek and a temporal proof, small on disk.
# ===========================================================================
if have_encoder libx265; then
    "$FFMPEG" -hide_banner -y -f lavfi -i 'testsrc2=s=1920x1080:r=24:d=6' \
      -an -c:v libx265 -preset faster -crf 30 -pix_fmt "$SRC8" $TAGS \
      -movflags +faststart "$MEDIA/motion_1920x1080_24fps_8bit_hevc.mp4" \
      || echo "FAILED: motion_1920x1080_24fps_8bit_hevc.mp4" >&2

    "$FFMPEG" -hide_banner -y -f lavfi -i 'testsrc2=s=1920x1080:r=24:d=6' \
      -an -c:v libx265 -preset faster -crf 30 -pix_fmt "$SRC10" $X265 \
      -profile:v main10 $TAGS -movflags +faststart \
      "$MEDIA/motion_1920x1080_24fps_10bit_hevc.mp4" \
      || echo "FAILED: motion_1920x1080_24fps_10bit_hevc.mp4" >&2

    "$FFMPEG" -hide_banner -y -f lavfi -i 'testsrc2=s=1920x1080:r=24000/1001:d=6' \
      -an -c:v libx265 -preset faster -crf 30 -pix_fmt "$SRC8" $TAGS \
      -movflags +faststart "$MEDIA/motion_1920x1080_23976fps_8bit_hevc.mp4" \
      || echo "FAILED: motion_1920x1080_23976fps_8bit_hevc.mp4" >&2
else
    echo "SKIP 4-6: the 1920x1080 motion clips (no libx265)" >&2
fi

# ===========================================================================
# 7. Full-range (pc) BT.709 at 1920x1080. The clips above are limited range
#    (tv), which is what lavfi and x265 default to; the range/matrix fields
#    this clip carries make a color-metadata gate impossible to evade by
#    only ever exercising limited-range input. x265 only writes
#    primaries/transfer into the VUI when they are given as encoder
#    parameters, so both channels are set: without them libmpv reports the
#    primaries as unknown and the fixture would not exercise the field.
# ===========================================================================
if have_encoder libx265; then
    "$FFMPEG" -hide_banner -y -f lavfi -i 'testsrc2=s=1920x1080:r=24:d=6' \
      -vf 'setparams=range=pc:color_primaries=bt709:color_trc=bt709:colorspace=bt709' \
      -an -c:v libx265 -preset faster -crf 30 -pix_fmt "$SRC8" $TAGS $X265 \
      -color_range pc -colorspace bt709 -color_primaries bt709 -color_trc bt709 \
      -movflags +faststart "$MEDIA/fullrange_1920x1080_24fps_8bit_hevc.mp4" \
      || echo "FAILED: fullrange_1920x1080_24fps_8bit_hevc.mp4" >&2
else
    echo "SKIP 7: fullrange_1920x1080_24fps_8bit_hevc.mp4 (no libx265)" >&2
fi

# ===========================================================================
# 8. Hard cut + saturated + near-black, with audio. 24 fps, 8-bit, 6 s:
#      0.000-2.000  motion, full chroma          (testsrc2)
#      2.000-4.000  saturated green, moving box  (hard cut at 2.000)
#      4.000-6.000  near-black, moving dim box   (hard cut at 4.000)
#    Audio: 440 Hz tone for the whole clip plus a 880 Hz tone from 2.000 s,
#    so an A/V sync check has something real to hear.
# ===========================================================================
if have_encoder libx265; then
    # Double-quoted so the inner single quotes survive exactly as the .cmd
    # passes them: the expressions' commas must stay ffmpeg-filter escapes,
    # not arguments of this shell.
    "$FFMPEG" -hide_banner -y \
      -f lavfi -i "testsrc2=s=320x180:r=24:d=2" \
      -f lavfi -i "color=c=0x00FF00:s=320x180:r=24:d=2,drawbox=x='mod(t*90\,320-30)':y=60:w=30:h=60:color=0xFFFFFF:t=fill" \
      -f lavfi -i "color=c=0x080808:s=320x180:r=24:d=2,drawbox=x='mod(t*90\,320-30)':y=60:w=30:h=60:color=0x404040:t=fill" \
      -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=6" \
      -f lavfi -i "sine=frequency=880:sample_rate=48000:duration=6,volume=enable='between(t\,2\,4)':volume=0.6" \
      -filter_complex "[0:v][1:v][2:v]concat=n=3:v=1:a=0[v];[3:a][4:a]amix=inputs=2:duration=first:normalize=0[a]" \
      -map "[v]" -map "[a]" -c:a aac -b:a 128k -ar 48000 -ac 2 -shortest \
      -c:v libx265 -preset faster -crf 26 -pix_fmt "$SRC8" $TAGS -movflags +faststart \
      "$MEDIA/cut_saturated_black_320x180_24fps_8bit_hevc.mp4" \
      || echo "FAILED: cut_saturated_black_320x180_24fps_8bit_hevc.mp4" >&2
else
    echo "SKIP 8: cut_saturated_black_320x180_24fps_8bit_hevc.mp4 (no libx265)" >&2
fi

# ===========================================================================
# 9. Height 1072 is even (a legal yuv420p height) and is NOT a multiple of 32
#    (1072 / 32 = 33.5), which is exactly the dyn-HW graph case: the padded
#    boundary must be derived from the source, not assumed.
# ===========================================================================
if have_encoder libx265; then
    "$FFMPEG" -hide_banner -y -f lavfi -i 'testsrc2=s=1920x1072:r=24:d=4' \
      -an -c:v libx265 -preset faster -crf 30 -pix_fmt "$SRC8" $TAGS \
      -movflags +faststart "$MEDIA/nonmult32_motion_1920x1072_24fps_8bit_hevc.mp4" \
      || echo "FAILED: nonmult32_motion_1920x1072_24fps_8bit_hevc.mp4" >&2
else
    echo "SKIP 9: nonmult32_motion_1920x1072_24fps_8bit_hevc.mp4 (no libx265)" >&2
fi

# ===========================================================================
# 10. Portrait 1080x1920: the rotate/fit and preparation-plan geometry case.
# ===========================================================================
if have_encoder libx265; then
    "$FFMPEG" -hide_banner -y -f lavfi -i 'testsrc2=s=1080x1920:r=24:d=4' \
      -an -c:v libx265 -preset faster -crf 30 -pix_fmt "$SRC8" $TAGS \
      -movflags +faststart "$MEDIA/portrait_motion_1080x1920_24fps_8bit_hevc.mp4" \
      || echo "FAILED: portrait_motion_1080x1920_24fps_8bit_hevc.mp4" >&2
else
    echo "SKIP 10: portrait_motion_1080x1920_24fps_8bit_hevc.mp4 (no libx265)" >&2
fi

# ===========================================================================
# 11. Real adaptive HLS: three real renditions, each a real media playlist
#     with real segments on disk. The master playlist itself is NOT written
#     here: mikai_http_fixture synthesises it per request so it can attach
#     the signed query strings (and the deliberately expired variant) that
#     the signed-URL recovery gate needs. (The batch file needs a doubled
#     percent in its segment template; a shell needs none.)
#       v180  320x180  8-bit H.264    low bandwidth rung
#       v360  640x360  8-bit H.264    mid rung
#       v720 1280x720 10-bit HEVC     top rung, exercises Main10 over HLS
# ===========================================================================
if have_encoder libx264; then
    "$FFMPEG" -hide_banner -y -f lavfi -i 'testsrc2=s=320x180:r=24:d=6' \
      -an -c:v libx264 -preset veryfast -crf 28 -pix_fmt "$SRC8" -g 48 -keyint_min 48 \
      -sc_threshold 0 -f hls -hls_time 2 -hls_playlist_type vod -hls_list_size 0 \
      -hls_segment_filename "$HLS/v180/seg%03d.ts" "$HLS/v180/playlist.m3u8" \
      || echo "FAILED: hls/v180" >&2

    "$FFMPEG" -hide_banner -y -f lavfi -i 'testsrc2=s=640x360:r=24:d=6' \
      -an -c:v libx264 -preset veryfast -crf 26 -pix_fmt "$SRC8" -g 48 -keyint_min 48 \
      -sc_threshold 0 -f hls -hls_time 2 -hls_playlist_type vod -hls_list_size 0 \
      -hls_segment_filename "$HLS/v360/seg%03d.ts" "$HLS/v360/playlist.m3u8" \
      || echo "FAILED: hls/v360" >&2
else
    echo "SKIP 11a: the 8-bit H.264 HLS rungs (no libx264)" >&2
fi
if have_encoder libx265; then
    "$FFMPEG" -hide_banner -y -f lavfi -i 'testsrc2=s=1280x720:r=24:d=6' \
      -an -c:v libx265 -preset faster -crf 30 -pix_fmt "$SRC10" $X265 -profile:v main10 \
      -g 48 -keyint_min 48 -sc_threshold 0 -f hls -hls_time 2 -hls_playlist_type vod \
      -hls_list_size 0 -hls_segment_filename "$HLS/v720/seg%03d.ts" \
      "$HLS/v720/playlist.m3u8" \
      || echo "FAILED: hls/v720" >&2
else
    echo "SKIP 11b: the 10-bit HEVC HLS rung (no libx265)" >&2
fi

# Author PQ samples at 10-bit precision. The right-edge highlight ramp
# distinguishes tone curves that legitimately preserve the same dark values.
if have_encoder libx265; then
    "$FFMPEG" -hide_banner -y -f lavfi \
      -i "nullsrc=s=1280x720:r=24:d=16,format=yuv420p10le,geq=lum='if(gt(X,W*0.96),548+175*(X/W-0.96)/0.04,244+140*X/W+4*sin(T*2*PI*0.1))':cb=512:cr=512" \
      -an -c:v libx265 -preset faster -crf 20 -pix_fmt "$SRC10" \
      -profile:v main10 \
      -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc \
      -color_range tv -x265-params 'log-level=error:pools=2:master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1):max-cll=1000,400' \
      $TAGS -movflags +faststart \
      "$MEDIA/hdr_dark_gradient_1280x720_24fps_10bit_pq_hevc.mp4" \
      || echo "FAILED: hdr_dark_gradient_1280x720_24fps_10bit_pq_hevc.mp4" >&2
else
    echo "SKIP 12: hdr_dark_gradient_1280x720_24fps_10bit_pq_hevc.mp4 (no libx265)" >&2
fi

# Lossless coding preserves the small shadow-noise signal; a lossy encode can
# remove it and turn an HDR-noise regression check into a constant-field test.
if have_encoder libx265; then
    "$FFMPEG" -hide_banner -y -f lavfi \
      -i "nullsrc=s=1280x720:r=24:d=16,format=yuv420p10le,geq=lum='if(gt(X,W*0.96),548+175*(X/W-0.96)/0.04,324+5*(2*random(1)-1))':cb=512:cr=512" \
      -an -c:v libx265 -preset faster -pix_fmt "$SRC10" \
      -profile:v main10 \
      -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc \
      -color_range tv -x265-params 'log-level=error:pools=2:lossless=1:master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1):max-cll=1000,400' \
      $TAGS -movflags +faststart \
      "$MEDIA/hdr_dark_noise_1280x720_24fps_10bit_pq_hevc.mp4" \
      || echo "FAILED: hdr_dark_noise_1280x720_24fps_10bit_pq_hevc.mp4" >&2
else
    echo "SKIP 13: hdr_dark_noise_1280x720_24fps_10bit_pq_hevc.mp4 (no libx265)" >&2
fi

# ===========================================================================
# 14. Moving source with an on-picture 8-bit frame counter barcode. The
#     FRAME_ID_DURATION_SECONDS environment variable extends this single
#     source epoch for long continuous-presentation acceptance runs.
# ===========================================================================
FRAME_ID_DURATION_SECONDS="${FRAME_ID_DURATION_SECONDS:-16}"
FRAME_ID_SIZE="${FRAME_ID_SIZE:-1280x720}"
case "$FRAME_ID_DURATION_SECONDS" in
    ''|*[!0-9]*)
        echo "FRAME_ID_DURATION_SECONDS must be a positive integer" >&2
        exit 2
        ;;
esac
if [ "$FRAME_ID_DURATION_SECONDS" -lt 1 ]; then
    echo "FRAME_ID_DURATION_SECONDS must be a positive integer" >&2
    exit 2
fi
case "$FRAME_ID_SIZE" in
    1280x720)
        frame_id_width=1280
        frame_id_height=720
        frame_id_name=motion_frame_id_1280x720_24fps_10bit_hevc.mp4
        ;;
    640x360)
        frame_id_width=640
        frame_id_height=360
        frame_id_name=continuous_frame_id_audio_640x360_24fps_10bit_hevc.mp4
        ;;
    *)
        echo "FRAME_ID_SIZE must be 1280x720 or 640x360" >&2
        exit 2
        ;;
esac
if have_encoder libx265; then
    frame_id_filter="testsrc2=s=${frame_id_width}x${frame_id_height}:r=24:d=$FRAME_ID_DURATION_SECONDS"
    for bit in 0 1 2 3 4 5 6 7; do
        x=$(((32 + bit * 24) * frame_id_width / 1280))
        y=$((16 * frame_id_height / 720))
        box_width=$((16 * frame_id_width / 1280))
        box_height=$((40 * frame_id_height / 720))
        frame_id_filter="$frame_id_filter,drawbox=x=$x:y=$y:w=$box_width:h=$box_height:color=black:t=fill,drawbox=x=$x:y=$y:w=$box_width:h=$box_height:color=white:t=fill:enable='gt(mod(floor(n/$((1 << bit))),2),0)'"
    done
    "$FFMPEG" -hide_banner -y -f lavfi -i "$frame_id_filter" \
      -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=$FRAME_ID_DURATION_SECONDS" \
      -map 0:v:0 -map 1:a:0 -c:v libx265 -preset faster -crf 20 -pix_fmt "$SRC10" $X265 \
      -profile:v main10 -color_primaries bt709 -color_trc bt709 -colorspace bt709 \
      -color_range tv -c:a aac -b:a 128k -ar 48000 -ac 2 -shortest \
      -color_range tv $TAGS -movflags +faststart "$MEDIA/$frame_id_name" \
      || echo "FAILED: $frame_id_name" >&2
else
    echo "SKIP 14: $frame_id_name (no libx265)" >&2
fi

# --- summary ---------------------------------------------------------------
# Partial local toolchains remain useful; CI acceptance lanes must check every
# required asset explicitly rather than treating a skipped encode as a pass.
echo
echo "media written to $MEDIA"
echo "hls    written to $HLS"
missing=0
for clip in \
  motion_320x180_24fps_8bit_hevc.mp4 \
  motion_320x180_23976fps_8bit_hevc.mp4 \
  motion_320x180_24fps_10bit_hevc.mp4 \
  motion_1920x1080_24fps_8bit_hevc.mp4 \
  motion_1920x1080_24fps_10bit_hevc.mp4 \
  motion_1920x1080_23976fps_8bit_hevc.mp4 \
  fullrange_1920x1080_24fps_8bit_hevc.mp4 \
  cut_saturated_black_320x180_24fps_8bit_hevc.mp4 \
  nonmult32_motion_1920x1072_24fps_8bit_hevc.mp4 \
  portrait_motion_1080x1920_24fps_8bit_hevc.mp4 \
  hdr_dark_gradient_1280x720_24fps_10bit_pq_hevc.mp4 \
  hdr_dark_noise_1280x720_24fps_10bit_pq_hevc.mp4 \
  "$frame_id_name"; do
    if [ -f "$MEDIA/$clip" ]; then
        echo "  present:  $clip"
    else
        echo "  MISSING:  $clip" >&2
        missing=$((missing + 1))
    fi
done
for rung in v180 v360 v720; do
    playlist="$HLS/$rung/playlist.m3u8"
    if [ ! -s "$playlist" ]; then
        echo "MISSING: HLS playlist $playlist" >&2
        missing=$((missing + 1))
        continue
    fi
    for segment in 000 001 002; do
        if [ ! -s "$HLS/$rung/seg$segment.ts" ]; then
            echo "MISSING: HLS segment $HLS/$rung/seg$segment.ts" >&2
            missing=$((missing + 1))
        fi
    done
done
if [ "$missing" -gt 0 ]; then
    echo "$missing required fixture(s) are missing; see the SKIP/FAILED lines above." >&2
    exit 1
fi
