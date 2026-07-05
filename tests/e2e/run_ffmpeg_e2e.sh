#!/usr/bin/env bash
# End-to-end: ffmpeg publish -> roqr-ingest -> roqr-relayd -> roqr-egress
# -> ffmpeg pull. Self-skips when ffmpeg/ffprobe are unavailable.
#
# Args: $1 = build bin dir (contains roqr-relayd/ingest/egress),
#       $2 = cert dir (cert.pem/key.pem), $3 = codec: "h264" or "hevc".
set -uo pipefail

BIN="$1"; CERTS="$2"; CODEC="${3:-h264}"

if ! command -v ffmpeg >/dev/null || ! command -v ffprobe >/dev/null; then
    echo "SKIP: ffmpeg/ffprobe not found on PATH"
    exit 0
fi

RELAY_PORT=45590
INGEST_RTMP=45591
EGRESS_RTMP=45592
STREAM="cam"
WORK="$(mktemp -d)"
OUT="${WORK}/out.flv"
PIDS=()
cleanup() {
    for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "${WORK}"
}
trap cleanup EXIT

case "$CODEC" in
    h264) VENC=(-c:v libx264 -preset ultrafast); PROBE="h264";;
    hevc)
        # Capture before grepping: piping ffmpeg directly into `grep -q`
        # lets grep close its stdin as soon as it matches, which sends
        # ffmpeg SIGPIPE (exit 141); with `pipefail` that then reports the
        # whole pipeline as failed even though grep found the match, and
        # this case would false-SKIP every time libx265 is actually there.
        ENCODERS="$(ffmpeg -hide_banner -encoders 2>/dev/null)"
        if ! grep -q libx265 <<<"${ENCODERS}"; then
            echo "SKIP: libx265 not available for the HEVC/E-RTMP case"
            exit 0
        fi
        # No explicit -tag:v: this ffmpeg's flv muxer rejects an
        # explicitly-set "hvc1"/"hev1" codec tag ("Tag hvc1 incompatible
        # with output codec id '173'") but auto-selects a working tag for
        # HEVC when none is given.
        VENC=(-c:v libx265); PROBE="hevc";;
    *) echo "unknown codec $CODEC"; exit 2;;
esac

"${BIN}/roqr-relayd" --cert "${CERTS}/cert.pem" --key "${CERTS}/key.pem" \
    --port "${RELAY_PORT}" --mode media &
PIDS+=($!)
sleep 0.5

"${BIN}/roqr-egress" --rtmp-port "${EGRESS_RTMP}" \
    --roqr-host 127.0.0.1 --roqr-port "${RELAY_PORT}" --stream "${STREAM}" &
PIDS+=($!)
"${BIN}/roqr-ingest" --rtmp-port "${INGEST_RTMP}" \
    --roqr-host 127.0.0.1 --roqr-port "${RELAY_PORT}" &
PIDS+=($!)
sleep 1

# Pull from egress into a file (runs in the background, bounded by -t and a
# hard wall-clock safety timeout). ffmpeg's live-RTMP demuxer needs several
# seconds of accumulated packet duration before avformat_find_stream_info()
# finalizes and it starts writing output; too short a publish window leaves
# it blocked probing forever (observed hang with a 3s publish/3s pull pair).
timeout 20 ffmpeg -hide_banner -loglevel error -y \
    -i "rtmp://127.0.0.1:${EGRESS_RTMP}/live/${STREAM}" \
    -t 4 -c copy "${OUT}" &
PULL_PID=$!
sleep 0.5

# Publish a 6s synthetic stream into ingest (long enough for the pull side's
# probe to finish before the publisher stops).
ffmpeg -hide_banner -loglevel error \
    -re -f lavfi -i "testsrc2=size=320x240:rate=15" -t 6 \
    "${VENC[@]}" -pix_fmt yuv420p -f flv \
    "rtmp://127.0.0.1:${INGEST_RTMP}/live/${STREAM}"

wait "${PULL_PID}"

if [ ! -s "${OUT}" ]; then
    echo "FAIL: egress produced no output"
    exit 1
fi
CODEC_SEEN="$(ffprobe -hide_banner -loglevel error \
    -select_streams v:0 -show_entries stream=codec_name \
    -of default=nw=1:nk=1 "${OUT}")"
echo "e2e output codec: ${CODEC_SEEN} (expected ${PROBE})"
if [ "${CODEC_SEEN}" != "${PROBE}" ]; then
    echo "FAIL: expected ${PROBE}, got ${CODEC_SEEN}"
    exit 1
fi
echo "PASS: ${CODEC} end-to-end through ingest -> relayd -> egress"
exit 0
