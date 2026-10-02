@echo off
rem ---------------------------------------------------------------------------
rem M02 hermetic fixture media generator.
rem
rem Every asset is produced from ffmpeg's own lavfi sources, so the matrix is
rem project-owned and licence-clean: no third-party footage is downloaded,
rem copied or re-encoded. Re-running this script reproduces the same inputs
rem (lavfi is deterministic; only the muxer timestamp container differs).
rem
rem Usage (from a cmd.exe, repo root):
rem     set FFMPEG=C:\path\to\ffmpeg.exe
rem     tests\fixtures\generate_media.cmd
rem
rem FFMPEG defaults to "ffmpeg" on PATH. No Python, no Conda, no network.
rem ---------------------------------------------------------------------------
setlocal

if not defined FFMPEG set "FFMPEG=ffmpeg"
set "HERE=%~dp0"
set "MEDIA=%HERE%media"
set "HLS=%HERE%hls"

echo using ffmpeg: %FFMPEG%
%FFMPEG% -hide_banner -version | findstr /C:"ffmpeg version"

if not exist "%MEDIA%" mkdir "%MEDIA%"
if not exist "%HLS%\v180" mkdir "%HLS%\v180"
if not exist "%HLS%\v360" mkdir "%HLS%\v360"
if not exist "%HLS%\v720" mkdir "%HLS%\v720"

set "SRC8=yuv420p"
set "SRC10=yuv420p10le"
set "TAGS=-tag:v hvc1"
set "X265=-x265-params log-level=error:pools=2"

rem --- source rationale ------------------------------------------------------
rem testsrc2 is used wherever "real motion, real chroma" is required: it has
rem genuine inter-frame motion plus saturated bars. A still frame would make
rem every later interpolation/temporal gate vacuous.
rem ===========================================================================
rem 1. The primary 320x180 clip: 24 fps, 8-bit HEVC, 12 s. The geometry and
rem    duration external_media_probe requires (320x180, >10 s, colourful).
rem ===========================================================================
%FFMPEG% -hide_banner -y -f lavfi -i "testsrc2=s=320x180:r=24:d=12" ^
  -an -c:v libx265 -preset faster -crf 26 -pix_fmt %SRC8% %TAGS% ^
  -movflags +faststart "%MEDIA%\motion_320x180_24fps_8bit_hevc.mp4"

rem 2. Same geometry and duration at 24000/1001 fps (23.976).
%FFMPEG% -hide_banner -y -f lavfi -i "testsrc2=s=320x180:r=24000/1001:d=12" ^
  -an -c:v libx265 -preset faster -crf 26 -pix_fmt %SRC8% %TAGS% ^
  -movflags +faststart "%MEDIA%\motion_320x180_23976fps_8bit_hevc.mp4"

rem 3. Same geometry at 10-bit HEVC (Main10), so bit-depth switching is real.
%FFMPEG% -hide_banner -y -f lavfi -i "testsrc2=s=320x180:r=24:d=12" ^
  -an -c:v libx265 -preset faster -crf 26 -pix_fmt %SRC10% %X265% ^
  -profile:v main10 %TAGS% -movflags +faststart ^
  "%MEDIA%\motion_320x180_24fps_10bit_hevc.mp4"

rem ===========================================================================
rem 4-6. 1920x1080 at 24 fps 8-bit, 24 fps 10-bit, and 24000/1001 8-bit.
rem    6 s each: enough for a seek and a temporal proof, small on disk.
rem ===========================================================================
%FFMPEG% -hide_banner -y -f lavfi -i "testsrc2=s=1920x1080:r=24:d=6" ^
  -an -c:v libx265 -preset faster -crf 30 -pix_fmt %SRC8% %TAGS% ^
  -movflags +faststart "%MEDIA%\motion_1920x1080_24fps_8bit_hevc.mp4"

%FFMPEG% -hide_banner -y -f lavfi -i "testsrc2=s=1920x1080:r=24:d=6" ^
  -an -c:v libx265 -preset faster -crf 30 -pix_fmt %SRC10% %X265% ^
  -profile:v main10 %TAGS% -movflags +faststart ^
  "%MEDIA%\motion_1920x1080_24fps_10bit_hevc.mp4"

%FFMPEG% -hide_banner -y -f lavfi -i "testsrc2=s=1920x1080:r=24000/1001:d=6" ^
  -an -c:v libx265 -preset faster -crf 30 -pix_fmt %SRC8% %TAGS% ^
  -movflags +faststart "%MEDIA%\motion_1920x1080_23976fps_8bit_hevc.mp4"

rem ===========================================================================
rem 7. Full-range (pc) BT.709 at 1920x1080. The clips above are limited range
rem    (tv), which is what lavfi and x265 default to; plan 7.4 has the native ABI
rem    carry `range` and `matrix` straight through from what libmpv reports, so
rem    a gate cannot be evaded by only ever exercising limited-range input.
rem    x265 only writes primaries/transfer into the VUI when they are given as
rem    encoder parameters, so both channels are set: without them libmpv reports
rem    the primaries as unknown and the fixture would not exercise the field.
rem ===========================================================================
%FFMPEG% -hide_banner -y -f lavfi -i "testsrc2=s=1920x1080:r=24:d=6" ^
  -vf "setparams=range=pc:color_primaries=bt709:color_trc=bt709:colorspace=bt709" ^
  -an -c:v libx265 -preset faster -crf 30 -pix_fmt %SRC8% %TAGS% %X265% ^
  -color_range pc -colorspace bt709 -color_primaries bt709 -color_trc bt709 ^
  -movflags +faststart "%MEDIA%\fullrange_1920x1080_24fps_8bit_hevc.mp4"

rem ===========================================================================
rem 8. Hard cut + saturated + near-black, with audio. 24 fps, 8-bit, 6 s:
rem      0.000-2.000  motion, full chroma          (testsrc2)
rem      2.000-4.000  saturated green, moving box  (hard cut at 2.000)
rem      4.000-6.000  near-black, moving dim box   (hard cut at 4.000)
rem    Audio: 440 Hz tone for the whole clip plus a 880 Hz tone from 2.000 s,
rem    so an A/V sync check has something real to hear.
rem ===========================================================================
%FFMPEG% -hide_banner -y ^
  -f lavfi -i "testsrc2=s=320x180:r=24:d=2" ^
  -f lavfi -i "color=c=0x00FF00:s=320x180:r=24:d=2,drawbox=x='mod(t*90\,320-30)':y=60:w=30:h=60:color=0xFFFFFF:t=fill" ^
  -f lavfi -i "color=c=0x080808:s=320x180:r=24:d=2,drawbox=x='mod(t*90\,320-30)':y=60:w=30:h=60:color=0x404040:t=fill" ^
  -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=6" ^
  -f lavfi -i "sine=frequency=880:sample_rate=48000:duration=6,volume=enable='between(t\,2\,4)':volume=0.6" ^
  -filter_complex "[0:v][1:v][2:v]concat=n=3:v=1:a=0[v];[3:a][4:a]amix=inputs=2:duration=first:normalize=0[a]" ^
  -map "[v]" -map "[a]" -c:a aac -b:a 128k -ar 48000 -ac 2 -shortest ^
  -c:v libx265 -preset faster -crf 26 -pix_fmt %SRC8% %TAGS% -movflags +faststart ^
  "%MEDIA%\cut_saturated_black_320x180_24fps_8bit_hevc.mp4"

rem ===========================================================================
rem 9. Height 1072 is even (a legal yuv420p height) and is NOT a multiple of 32
rem    (1072 / 32 = 33.5), which is exactly the dyn-HW graph case: the padded
rem    boundary must be derived from the source, not assumed.
rem ===========================================================================
%FFMPEG% -hide_banner -y -f lavfi -i "testsrc2=s=1920x1072:r=24:d=4" ^
  -an -c:v libx265 -preset faster -crf 30 -pix_fmt %SRC8% %TAGS% ^
  -movflags +faststart "%MEDIA%\nonmult32_motion_1920x1072_24fps_8bit_hevc.mp4"

rem ===========================================================================
rem 10. Portrait 1080x1920: the rotate/fit and preparation-plan geometry case.
rem ===========================================================================
%FFMPEG% -hide_banner -y -f lavfi -i "testsrc2=s=1080x1920:r=24:d=4" ^
  -an -c:v libx265 -preset faster -crf 30 -pix_fmt %SRC8% %TAGS% ^
  -movflags +faststart "%MEDIA%\portrait_motion_1080x1920_24fps_8bit_hevc.mp4"

rem ===========================================================================
rem 11. Real adaptive HLS: three real renditions, each a real media playlist
rem     with real segments on disk. The master playlist itself is NOT written
rem     here: mikai_http_fixture synthesises it per request so it can attach the
rem     signed query strings (and the deliberately expired variant) that the
rem     signed-URL recovery gate needs. Note the doubled percent in the segment
rem     template: in a batch file a bare %0 expands to this script's path.
rem       v180  320x180  8-bit H.264    low bandwidth rung
rem       v360  640x360  8-bit H.264    mid rung
rem       v720 1280x720 10-bit HEVC     top rung, exercises Main10 over HLS
rem ===========================================================================
%FFMPEG% -hide_banner -y -f lavfi -i "testsrc2=s=320x180:r=24:d=6" ^
  -an -c:v libx264 -preset veryfast -crf 28 -pix_fmt %SRC8% -g 48 -keyint_min 48 ^
  -sc_threshold 0 -f hls -hls_time 2 -hls_playlist_type vod -hls_list_size 0 ^
  -hls_segment_filename "%HLS%\v180\seg%%03d.ts" "%HLS%\v180\playlist.m3u8"

%FFMPEG% -hide_banner -y -f lavfi -i "testsrc2=s=640x360:r=24:d=6" ^
  -an -c:v libx264 -preset veryfast -crf 26 -pix_fmt %SRC8% -g 48 -keyint_min 48 ^
  -sc_threshold 0 -f hls -hls_time 2 -hls_playlist_type vod -hls_list_size 0 ^
  -hls_segment_filename "%HLS%\v360\seg%%03d.ts" "%HLS%\v360\playlist.m3u8"

%FFMPEG% -hide_banner -y -f lavfi -i "testsrc2=s=1280x720:r=24:d=6" ^
  -an -c:v libx265 -preset faster -crf 30 -pix_fmt %SRC10% %X265% -profile:v main10 ^
  -g 48 -keyint_min 48 -sc_threshold 0 -f hls -hls_time 2 -hls_playlist_type vod ^
  -hls_list_size 0 -hls_segment_filename "%HLS%\v720\seg%%03d.ts" ^
  "%HLS%\v720\playlist.m3u8"

echo.
echo media written to %MEDIA%
echo hls    written to %HLS%
endlocal
