#!/bin/bash

# Check for the --record flag
RECORD_MODE=false
if [[ "$1" == "--record" ]]; then
    RECORD_MODE=true
    echo "RECORDING ENABLED"
fi

WEBCAM_DEV=$(v4l2-ctl --list-devices | grep "Logitech BRIO" -A 1 | tail -n 1 | xargs)
echo "Selected device: $WEBCAM_DEV"

# --- Hardware Level Fixes ---
v4l2-ctl -d $WEBCAM_DEV --set-ctrl=auto_exposure=1
v4l2-ctl -d $WEBCAM_DEV --set-ctrl=exposure_time_absolute=400
v4l2-ctl -d $WEBCAM_DEV --set-ctrl=gain=0
v4l2-ctl -d $WEBCAM_DEV --set-ctrl=backlight_compensation=0
v4l2-ctl -d $WEBCAM_DEV --set-ctrl=zoom_absolute=300
v4l2-ctl -d $WEBCAM_DEV --set-ctrl=pan_absolute=3600
v4l2-ctl -d $WEBCAM_DEV --set-ctrl=tilt_absolute=3600


# --- Launch HDMI Feed ---
if [ "$RECORD_MODE" = true ]; then
    # Path with Recording: ffmpeg encodes and splits to file + ffplay
    FILENAME="$HOME/Downloads/stage_$(date +%Y%m%d_%H%M%S).mp4"
    echo "Saving to: $FILENAME"
    
    ffmpeg -hide_banner -loglevel error \
       -f v4l2 -input_format mjpeg -framerate 30 -video_size 1280x720 -i $WEBCAM_DEV \
       -f pulse -channels 32 -i default \
       -vf "crop=0.45*iw:0.45*ih:0.35*iw:0.35*ih" \
       -af "pan=mono|c0=c31" \
       -c:v libx264 -preset medium -crf 25 -tune zerolatency \
       -c:a aac -b:a 128k \
       -f tee -map 0:v -map 1:a "$FILENAME|[f=nut]pipe:" | \
       ffplay pipe: -left 1920 -top 0 -fs -alwaysontop -noborder -loglevel error &
else
    # Standard Path: Direct display for lowest latency
    ffplay -f v4l2 -input_format mjpeg -framerate 30 -video_size 1280x720 \
           -threads 4 -flags low_delay -i $WEBCAM_DEV \
           -vf "crop=1*iw:1*ih:1*iw:1*ih" \
           -left 1920 -top 0 -fs -loglevel error &
fi
PID1=$!

# --- Start the "little window" preview on the main monitor ---
ffplay -f x11grab -probesize 32M -framerate 15 -video_size 1920x1080 \
       -flags low_delay -framedrop -i :1.0+1920,0 \
       -vf "scale=480:-1" -alwaysontop -loglevel error &
PID2=$!

# Function to kill both processes on Ctrl+C
cleanup() {
    echo -e "\nShutting down feeds..."
    # Using SIGTERM to allow ffmpeg to close the MP4 file header correctly
    kill -SIGTERM $PID1 $PID2 2>/dev/null
    exit
}

trap cleanup SIGINT

echo "Feeds are running. Press Ctrl+C to exit."
wait
