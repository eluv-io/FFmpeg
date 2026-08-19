#!/usr/bin/env bash
#
# ELUVIO fork-local test: AC-4 (dac4) fMP4 remux passthrough.
#
# This file is intentionally self-contained and lives only on the eluv-io fork
# (a NEW path upstream FFmpeg does not have), so it never conflicts when the
# fork is rebased/merged onto a newer upstream tag. Do NOT wire it into FATE
# (tests/Makefile / tests/ref) -- that would edit upstream-tracked files and
# reintroduce rebase friction. Run it directly, or from the elv-toolchain test
# task, against a built+installed fork.
#
# What it verifies (the AC-4 mux/demux additions in libavformat/movenc.c and
# libavformat/mov.c): `ffmpeg -c:a copy` on an AC-4-in-MP4 source produces, for
# every MP4-based delivery format, an `ac-4` sample entry whose `dac4` config
# box is byte-identical to the source. AC-4 is passthrough-only (no encoder),
# so a stream-copy is the exact and complete acceptance test.
#
# Exit codes: 0 = pass, 1 = fail, 77 = skipped (prerequisites absent) -- 77 is
# the FATE/automake convention so a CI harness treats a missing-asset run as a
# skip, not a failure.
#
# Assets come from the shared eluvio-test-assets bucket (a flat mirror), so this
# test uses the same media any CI runner already syncs -- no Dolby-kit checkout
# needed. Populate the mirror with avpipe's scripts/download-test-assets.sh
# (gsutil rsync gs://eluvio-test-assets -> a local dir), then point ELV_TEST_ASSETS_DIR
# at it. Files used: the Audio_ID_*_ac4.mp4 / sample_ac4_*.mp4 set (stereo, 5.1,
# 5.1.4, IMS/Atmos, audio-only and with H.264) plus
# Audio_ID_720p_50fps_h264_6ch_640kbps_ddp_joc.mp4 (EAC3 regression). Any AC-4
# sample missing from the mirror is reported as a skip line, not a failure.
#
# Options:
#   -k, --keep        keep the temp work dir (muxed outputs) instead of deleting
#                     it on exit; the path is printed so you can inspect them
#   -h, --help        show usage
#
# Overridable via env:
#   FFMPEG_BIN        path to ffmpeg   (default: built binary next to tests/, else PATH)
#   ELV_TEST_ASSETS_DIR  eluvio-test-assets mirror (no default; required unless the
#                     sample paths below are set explicitly)
#   AC4_SAMPLE        AC-4 file  (default: $ELV_TEST_ASSETS_DIR/Audio_ID_6ch_128kbps_25fps_ac4.mp4)
#   EC3_SAMPLE        EAC3 file  (default: $ELV_TEST_ASSETS_DIR/Audio_ID_720p_50fps_h264_6ch_640kbps_ddp_joc.mp4)

set -u

# --- options -----------------------------------------------------------------
keep=0
while [ $# -gt 0 ]; do
    case "$1" in
        -k|--keep) keep=1 ;;
        -h|--help)
            sed -n '2,/^set -u/p' "$0" | sed 's/^# \{0,1\}//; /^set -u/d'
            exit 0 ;;
        *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
    shift
done

# --- locate ffmpeg -----------------------------------------------------------
script_dir=$(cd "$(dirname "$0")" && pwd)
ffmpeg_default="$script_dir/../../ffmpeg"   # tests/elv/ -> repo root
FFMPEG_BIN=${FFMPEG_BIN:-}
if [ -z "$FFMPEG_BIN" ]; then
    if [ -x "$ffmpeg_default" ]; then FFMPEG_BIN="$ffmpeg_default"
    elif command -v ffmpeg >/dev/null 2>&1; then FFMPEG_BIN=$(command -v ffmpeg)
    fi
fi
if [ -z "$FFMPEG_BIN" ] || [ ! -x "$FFMPEG_BIN" ]; then
    echo "SKIP: no ffmpeg binary found (set FFMPEG_BIN)"; exit 77
fi

# --- locate samples (from the eluvio-test-assets mirror) ---------------------
ELV_TEST_ASSETS_DIR=${ELV_TEST_ASSETS_DIR:-}
ac4_primary=${AC4_SAMPLE:-}
if [ -z "$ac4_primary" ]; then
    if [ -z "$ELV_TEST_ASSETS_DIR" ]; then
        echo "SKIP: ELV_TEST_ASSETS_DIR not set (eluvio-test-assets mirror)"
        echo "      (set it, or set AC4_SAMPLE directly; sync gs://eluvio-test-assets"
        echo "       via avpipe's scripts/download-test-assets.sh)"; exit 77
    fi
    ac4_primary=$ELV_TEST_ASSETS_DIR/Audio_ID_6ch_128kbps_25fps_ac4.mp4
fi
if [ ! -f "$ac4_primary" ]; then
    echo "SKIP: AC-4 sample not found at $ac4_primary"
    echo "      (set ELV_TEST_ASSETS_DIR/AC4_SAMPLE, or sync gs://eluvio-test-assets"
    echo "       via avpipe's scripts/download-test-assets.sh)"; exit 77
fi

command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 required for dac4 byte-compare"; exit 77; }

work=$(mktemp -d "${TMPDIR:-/tmp}/ac4-remux.XXXXXX")
if [ "$keep" -eq 1 ]; then
    trap 'echo; echo "outputs kept in: $work"' EXIT
else
    trap 'rm -rf "$work"' EXIT
fi

pass=0 fail=0
ff() { "$FFMPEG_BIN" -hide_banner -y "$@" >/dev/null 2>"$work/err"; }
ok()   { echo "  ok   - $1"; pass=$((pass+1)); }
bad()  { echo "  FAIL - $1"; [ -n "${2:-}" ] && sed 's/^/         /' "$2"; fail=$((fail+1)); }

# Extract the dac4 box payload (bytes after the 8-byte box header) and compare
# two files' payloads. Prints "MATCH", "DIFFER", or "MISSING <which>".
dac4_cmp() {
python3 - "$1" "$2" <<'PY'
import sys
def payload(path):
    d = open(path, "rb").read()
    i = d.find(b"dac4")
    if i < 4: return None
    size = int.from_bytes(d[i-4:i], "big")          # box size incl. 8-byte header
    return d[i+4 : i-4+size]                          # payload only
a, b = payload(sys.argv[1]), payload(sys.argv[2])
if a is None: print("MISSING src"); sys.exit(0)
if b is None: print("MISSING out"); sys.exit(0)
print("MATCH" if a == b else "DIFFER")
PY
}

# Assert: command succeeded, output <out> exists non-empty, and its dac4 matches <src>.
assert_dac4() {
    local label="$1" src="$2" out="$3"
    if [ ! -s "$out" ]; then bad "$label (no/empty output)" "$work/err"; return; fi
    local r; r=$(dac4_cmp "$src" "$out")
    case "$r" in
        MATCH) ok "$label (dac4 byte-identical to source)";;
        *)     bad "$label (dac4 $r)";;
    esac
}

echo "ffmpeg:  $FFMPEG_BIN"
echo "sample:  $ac4_primary"
echo

# Each test writes into its own subdir under $work so that, with --keep, the
# outputs are easy to tell apart (DASH and HLS both emit generic out*.m4s /
# init* names that would otherwise collide in a flat dir).
outdir() { mkdir -p "$work/$1"; echo "$work/$1"; }

# --- 1) the four MP4-based delivery formats, primary 5.1 sample --------------
echo "[delivery formats] -c:a copy, dac4 must survive:"

d=$(outdir mp4)
ff -i "$ac4_primary" -c:a copy "$d/out.mp4"
assert_dac4 "plain MP4" "$ac4_primary" "$d/out.mp4"

d=$(outdir dash)
ff -i "$ac4_primary" -c:a copy -f dash "$d/out.mpd"
assert_dac4 "DASH init" "$ac4_primary" "$d"/init-stream*.m4s

d=$(outdir hls)
ff -i "$ac4_primary" -c:a copy -f hls -hls_segment_type fmp4 \
   -hls_fmp4_init_filename init.mp4 "$d/out.m3u8"
assert_dac4 "HLS fMP4 init" "$ac4_primary" "$d/init.mp4"

# segment fMP4 with empty_moov and WITHOUT delay_moov: the crux case. AC-4's
# dac4 is verbatim extradata (present at header time), unlike EAC3's dec3 which
# is rebuilt from parsed packets and needs delay_moov -- so this must succeed.
d=$(outdir segment)
ff -i "$ac4_primary" -c:a copy -f segment -segment_time 30 -segment_format mp4 \
   -segment_format_options movflags=+frag_keyframe+empty_moov+default_base_moof \
   "$d/seg%03d.mp4"
assert_dac4 "segment fMP4 (empty_moov, no delay_moov)" "$ac4_primary" "$d/seg000.mp4"

# --- 2) config coverage: the other AC-4 presentations in the bucket -----------
# dac4 is verbatim extradata, so a stream-copy must reproduce it byte-for-byte
# regardless of presentation (stereo / 5.1 / 5.1.4 / IMS), frame rate, or
# whether the file also carries video. Audio-only remux (-map 0:a:0) so the
# A/V files don't drag their H.264 through an encode. Samples absent from the
# mirror are reported as skips, not failures -- add more by dropping them in
# gs://eluvio-test-assets and extending this list.
echo
echo "[config coverage] -c:a copy to MP4, per AC-4 presentation:"

i=0
while IFS='|' read -r fname label; do
    [ -n "$fname" ] || continue
    i=$((i+1))
    src=${ELV_TEST_ASSETS_DIR:+$ELV_TEST_ASSETS_DIR/$fname}
    if [ -z "$src" ] || [ ! -f "$src" ]; then
        echo "  skip - $label ($fname not in mirror)"; continue
    fi
    d=$(outdir "cfg$i")
    ff -i "$src" -map 0:a:0 -c:a copy "$d/out.mp4"
    assert_dac4 "$label" "$src" "$d/out.mp4"
# Fed by heredoc (not a pipe) so the loop runs in this shell and the pass/fail
# counters survive it.
done <<'CONFIGS'
Audio_ID_2ch_64kbps_25fps_ac4.mp4|stereo, 64k, 25fps
Audio_ID_6ch_128kbps_25fps_ac4.mp4|5.1, 128k, 25fps
Audio_ID_514ch_192kbps_25fps_ac4.mp4|5.1.4, 192k, 25fps
Audio_ID_ims_112kbps_25fps_ac4.mp4|IMS, 112k, 25fps
Audio_ID_720p_25fps_h264_6ch_128kbps_ac4.mp4|5.1 + h264, 25fps
Audio_ID_720p_50fps_h264_514ch_192kbps_ac4_fra.mp4|5.1.4 + h264, 50fps, fra
sample_ac4_atmos_10s.mp4|Atmos (IMS), 10s
sample_ac4_ims_nonatmos.mp4|IMS non-Atmos
CONFIGS

# --- 3) EAC3 regression: the dispatch insertion must not disturb dec3 ---------
# Uses the shared EAC3/Atmos asset (A/V), so map audio-only to avoid re-encoding
# its H.264 video.
EC3_SAMPLE=${EC3_SAMPLE:-${ELV_TEST_ASSETS_DIR:+$ELV_TEST_ASSETS_DIR/Audio_ID_720p_50fps_h264_6ch_640kbps_ddp_joc.mp4}}
if [ -f "$EC3_SAMPLE" ]; then
    echo
    echo "[regression] EAC3 dec3 still preserved:"
    d=$(outdir ec3)
    ff -i "$EC3_SAMPLE" -map 0:a:0 -c:a copy "$d/ec3.mp4"
    if [ -s "$d/ec3.mp4" ] && command -v mp4dump >/dev/null 2>&1; then
        if mp4dump "$d/ec3.mp4" | grep -qi 'dec3'; then ok "EAC3 dec3 present"
        else bad "EAC3 dec3 missing"; fi
    elif [ -s "$d/ec3.mp4" ]; then
        # no mp4dump: fall back to a raw box-name check
        if grep -qa 'dec3' "$d/ec3.mp4"; then ok "EAC3 dec3 present (raw)"
        else bad "EAC3 dec3 missing (raw)"; fi
    else
        bad "EAC3 remux produced no output" "$work/err"
    fi
fi

echo
echo "== $pass passed, $fail failed =="
[ "$fail" -eq 0 ]
