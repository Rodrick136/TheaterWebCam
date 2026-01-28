#!/bin/bash

# Usage: detect_white_frames.sh <input_video>
# Scans ffmpeg's signalstats output and reports frames where YAVG >= 254.9

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "Usage: $0 <input_video> [yavg_threshold]"
    echo "Example: $0 video.mp4 254.0"
    exit 1
fi

INPUT_VIDEO="$1"
THRESHOLD=${2:-254.0}
OUTPUT_DIR="frames"
LOG_FILE="white_frame_log.txt"

mkdir -p "$OUTPUT_DIR"
rm -f "$LOG_FILE"

# Run ffmpeg to emit per-frame signalstats to stderr.
# Use metadata=print to force filter metadata (lavfi.signalstats.*) to be printed.
ffmpeg -hide_banner -i "$INPUT_VIDEO" -vf "signalstats,metadata=print" -an -f null - 2> "$LOG_FILE"

# Parse the log file sequentially and associate nearby 'n:' and 'pts_time' values
# with the following YAVG entry. This handles metadata=print which emits
# separate lines like 'n: 12', 'pts_time: 5.123456' and 'lavfi.signalstats.YAVG=255'.
if ! grep -q "YAVG" "$LOG_FILE"; then
    echo "No frames with YAVG metadata found in ffmpeg output."
    exit 0
fi

MATCHES=$(awk -v TH="$THRESHOLD" '
    # ffmpeg prints "frame:" lines; remember the last frame seen
    {
        # robustly extract "frame:NNN" or "n: NNN"
        if (match($0, /frame[: ]*[0-9]+/)) {
            s = substr($0, RSTART, RLENGTH); gsub(/[^0-9]/, "", s); frame_idx = s; last_frame_idx = frame_idx
        } else if (match($0, /n[: ]*[0-9]+/)) {
            s = substr($0, RSTART, RLENGTH); gsub(/[^0-9]/, "", s); frame_idx = s; last_frame_idx = frame_idx
        }
    }
    /pts_time[:=]/ { if (match($0,/pts_time[:= ]*([0-9.]+)/,m)) pts_time=m[1] }
    /YAVG/ {
        # Prefer explicit YAVG=... capture (avoids matching earlier numbers in the line)
        if (match($0,/YAVG[:=]([0-9]+\.?[0-9]*)/,m2)) {
            yavg = m2[1]
        } else if (match($0,/([0-9]+\.?[0-9]*)/ , mval)) {
            yavg = mval[1]
        } else {
            next
        }
        if ((yavg+0) >= (TH+0)) {
            # prefer explicit frame number, fall back to last seen frame, else '-'
            if (frame_idx != "") out_frame = frame_idx
            else if (last_frame_idx != "") out_frame = last_frame_idx
            else out_frame = "-"
            out_pts = (pts_time == "" ? "-" : pts_time)
            print out_frame " " out_pts " YAVG=" yavg
        }
        # clear per-frame values to avoid reusing them
        frame_idx=""; pts_time=""; yavg=""
    }
' "$LOG_FILE")

if [ -z "$MATCHES" ]; then
    echo "No all-white frames detected."
    exit 0
fi

# Get video duration in seconds (float)
DURATION=$(ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 "$INPUT_VIDEO" 2>/dev/null)
if [ -z "$DURATION" ]; then
    DURATION=0
fi

echo "All-white frames detected (frame pts_time YAVG):"
echo
printf "%10s | %10s | %12s | %8s | %s\n" "Frame" "Seconds" "Timecode" "Rel%" "YAVG"
printf "%s\n" "--------------------------------------------------------------------------------"

while IFS= read -r line; do
    # line format: <frame> <pts_time> YAVG=<value>
    frame=$(echo "$line" | awk '{print $1}')
    pts_time=$(echo "$line" | awk '{print $2}')
    yavg=$(echo "$line" | sed -n 's/.*YAVG=\([0-9.]*\).*/\1/p')
    # normalize pts_time
    if [ "$pts_time" = "-" ] || [ -z "$pts_time" ]; then
        pts=0
    else
        pts=$pts_time
    fi

    # format timecode HH:MM:SS.mmm
    timecode=$(awk -v t="$pts" 'BEGIN{h=int(t/3600); m=int(t/60)%60; s=t - h*3600 - m*60; printf("%02d:%02d:%06.3f", h, m, s)}')

    if [ "$DURATION" = "0" ] || [ -z "$DURATION" ]; then
        rel="N/A"
    else
        rel=$(awk -v t="$pts" -v d="$DURATION" 'BEGIN{printf("%.2f", (t/d*100))}')
    fi

    printf "%10s | %10s | %12s | %7s%% | %s\n" "$frame" "$pts" "$timecode" "$rel" "$yavg"
done <<< "$MATCHES"

