import { mkdirSync } from "node:fs";
import { parseArgs } from "node:util";

// make sure we have the right commands available
const v4l2_check = (await Bun.$`which v4l2-ctl`.text()).trim();
if (v4l2_check.startsWith("which") === true) {
  console.error("v4l2-ctl command not found. Please install v4l2-ctl.");
  process.exit(1);
}
const gst_check = (await Bun.$`which gst-launch-1.0`.text()).trim();
if (gst_check.startsWith("which") === true) {
  console.error("gst-launch-1.0 command not found. Please install GStreamer.");
  process.exit(1);
}
const pw_record_check = (await Bun.$`which pw-record`.text()).trim();
if (pw_record_check.startsWith("which") === true) {
  console.error("pw-record command not found. Please install PipeWire.");
  process.exit(1);
}

// Parse command-line arguments
const { values, positionals } = parseArgs({
  args: Bun.argv,
  options: {
    record: {
      type: "boolean",
      short: "r",
      default: false,
    },
    focus: {
      type: "string",
      short: "f",
    },
    "video-size": {
      type: "string",
      default: "1920x1080",
    },
    framerate: {
      type: "string",
      default: "30",
    },
  },
  strict: true,
  allowPositionals: true,
});
const RECORD = values.record;

const FOCUS = values.focus ? Number.parseInt(values.focus) : undefined;
if (values.focus && Number.isNaN(FOCUS)) {
  console.log("Arg --focus has to be an integer");
  process.exit(1);
}

const VIDEO_SIZE = values["video-size"];
const vs_regex = /^\d+x\d+$/;
if (vs_regex.test(VIDEO_SIZE) === false) {
  console.log("Arg --video-size has to meet pattern /^\\d+x\\d+$/");
  process.exit(1);
}
const wxh = VIDEO_SIZE.split("x") as [string, string];
const WIDTH = Number.parseInt(wxh[0], 10);
const HEIGHT = Number.parseInt(wxh[1], 10);

const FRAMERATE = Number.parseInt(values.framerate, 10);
// has to be 30 or 60
if (FRAMERATE !== 30 && FRAMERATE !== 60) {
  console.log("Arg --framerate has to be either 30 or 60");
  process.exit(1);
}

console.log("Starting Webcam~");
if (RECORD) {
  console.log("Recording mode enabled");
}

const v4l2_DEVICE = (
    await Bun.$`v4l2-ctl --list-devices | grep "Logitech BRIO" -A 1 | tail -n 1 | xargs`.text()
  ).trim();
const devices = JSON.parse((await Bun.$`pw-dump`.text()).trim());
const webcams = [];
for (const device of devices) {
  if (device.type === "PipeWire:Interface:Node") {
    const props = device.info.props;
    if (props["object.path"] === `v4l2:${v4l2_DEVICE}`) {
      //console.log("Found webcam:", props);
      webcams.push({
        id: device.id,
        description: props["node.description"],
        node_name: props["node.name"],
      });
    }
  }
}
if (webcams.length === 0) {
  console.error("No webcams found. Please connect a webcam.");
  process.exit(1);
}
//console.log("Available webcams:", webcams);
const DEVICE = webcams[0]!.node_name as string;

console.log(`Selected cam device: ${webcams[0]?.description} (${DEVICE})`);
console.log(`Video size: ${VIDEO_SIZE}`);
console.log(`Framerate: ${FRAMERATE}`);

// Recording folder name
const DIR_NAME = `Recording__${new Date().toISOString()}`;
// Create recording directory if recording is enabled
if (RECORD) {
  mkdirSync(DIR_NAME);
}

let _videoProc: Bun.Subprocess<"pipe", "inherit", "inherit"> | null = null;
if (RECORD) {
  try {
    _videoProc = Bun.spawn({
      env: {
        ...process.env,
        GST_DEBUG: "3",
      },
      cmd: [
  "gst-launch-1.0", "-v", "-e",
  // 1. Add a small latency to the muxer so it doesn't choke waiting for audio
  "matroskamux",
    "name=mux",
    //"offset-to-zero=true",
    //"latency=200000000",
    "!",
  "filesink",
    `location=./${DIR_NAME}/webcam_full.mkv`,
    //"async=false",

  // --- VIDEO SOURCE ---
  "pipewiresrc",
    `target-object=${DEVICE}`,
    "do-timestamp=true",
    "!",
  `image/jpeg,width=${WIDTH},height=${HEIGHT},framerate=${FRAMERATE}/1`,
    "!",
  "queue",
    "leaky=downstream",
    "max-size-buffers=2",
    "!",
  "jpegdec", "!",
  "videorate",
    //"drop-only=true",
    "skip-to-first=true",
    "!",
  `video/x-raw,framerate=${FRAMERATE}/1`, // IMPORTANT: Force the framerate to be stable before the tee
    "!",
  "videoconvert",
    "!",
  "tee",
    "name=t",
    // --- VIDEO BRANCHES ---

    // Branch 1: Preview (queue is mandatory here to unblock the tee)
    "t.",
      "!",
    "queue",
      "leaky=downstream",
      "!", 
    "autovideosink",
      "sync=false",
      // "async=false", no such thing for sink

    // Branch 2: Recording
    /* "t.",
      "!",
    "queue",
      "max-size-buffers=300",
      "!", 
    "x264enc",
      "speed-preset=ultrafast",
      "tune=zerolatency",
      "bitrate=8192",
      "!", 
    "h264parse",
      "!",
    "mux.video_0", */

  // --- VOICE AUDIO ---
  "pipewiresrc",
    "client-name=TheaterWebCam",
    "stream-properties=props,media.name=voice",
    "do-timestamp=true",
    "!",
  "audio/x-raw,channels=1,rate=48000", // <--- CAPS FORCE MONO
    "!", 
  "queue",
    "leaky=2",
    "max-size-time=3000000000",
    "max-size-buffers=0",
    "!",
  "audioconvert", // convert to raw audio
    "!", 
  "audioresample", // resample if needed
    "!",
  "mux.audio_0", // end branch by connecting to muxer

  // --- EFFECTS AUDIO ---
  "pipewiresrc",
    "client-name=TheaterWebCam",
    "stream-properties=props,media.name=effects",
    "do-timestamp=true",
    "!",
  "audio/x-raw,channels=2,rate=48000", // <--- CAPS FORCE MONO
    "!", 
  "queue",
    "leaky=2",
    "max-size-time=3000000000",
    "max-size-buffers=0",
    "!",
  "audioconvert", // convert to raw audio
    "!", 
  "audioresample", // resample if needed
    "!",
  "mux.audio_1", // end branch by connecting to muxer
],
      stdout: "inherit",
      stdin: "pipe",
      stderr: "inherit",
    });
  } catch (e) {
    console.error("Failed to start effects recorder:", e);
  }
} else {
  // just play the video without recording
  try {
    _videoProc = Bun.spawn({
      cmd: [
        "gst-launch-1.0",
        "v4l2src",
        `device=${DEVICE}`,
        "!",
        `image/jpeg,width=${WIDTH},height=${HEIGHT},framerate=${FRAMERATE}/1`,
        "!",
        "jpegdec",
        "!",
        "videoconvert",
        "qos=true",
        "!",
        "queue",
        "max-size-buffers=2",
        "max-size-bytes=0",
        "max-size-time=0",
        "leaky=downstream",
        "!",
        "autovideosink",
        "sync=false",
      ],
      stdout: "inherit",
      stdin: "pipe",
      stderr: "inherit",
    });
  } catch (e) {
    console.error("Failed to start effects recorder:", e);
  }
}

const exit = async () => {
  console.log("Exiting, stopping subprocesses...");
  if (_videoProc) {
    console.log("Stopping video process...");
    _videoProc.kill("SIGINT");
    await _videoProc.exited;
    console.log("Video process stopped.");
  }
  console.log("All subprocesses stopped.");
};

// stop everything and await exit
process.on("SIGINT", async () => {
  console.log("SIGINT received");
  await exit();
  process.exit();
});
process.on("SIGTERM", async () => {
  console.log("SIGTERM received");
  await exit();
  process.exit();
});
process.on("uncaughtException", async (err) => {
  console.error("Uncaught exception:", err);
  await exit();
  process.exit(1);
});
process.on("unhandledRejection", async (reason, promise) => {
  console.error("Unhandled rejection at:", promise, "reason:", reason);
  await exit();
  process.exit(1);
});

console.log("Webcam streaming started. Press Ctrl+C to stop.");
// the process will keep running until sub processes are killed

{
  // give the cam some time to start up
  await new Promise((r) => setTimeout(r, 500));
  //configure the web cam settings
  //Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=white_balance_automatic=0`;
  //Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=white_balance_temperature=4500`;

  await Bun.$`v4l2-ctl -d ${v4l2_DEVICE} --set-ctrl=auto_exposure=1`;
  await Bun.$`v4l2-ctl -d ${v4l2_DEVICE} --set-ctrl=exposure_time_absolute=400`;
  await Bun.$`v4l2-ctl -d ${v4l2_DEVICE} --set-ctrl=gain=0`;
  await Bun.$`v4l2-ctl -d ${v4l2_DEVICE} --set-ctrl=backlight_compensation=0`;
  await Bun.$`v4l2-ctl -d ${v4l2_DEVICE} --set-ctrl=zoom_absolute=300`;

  if (FOCUS !== undefined) {
    console.log(`Focus set to: ${FOCUS}`);
    await Bun.$`v4l2-ctl -d ${v4l2_DEVICE} --set-ctrl=focus_automatic_continuous=0`;
    await Bun.$`v4l2-ctl -d ${v4l2_DEVICE} --set-ctrl=focus_absolute=${FOCUS}`;
  }
  //Bun.$`v4l2-ctl -d ${v4l2_DEVICE} --set-ctrl=pan_absolute=3600`;
  //Bun.$`v4l2-ctl -d ${v4l2_DEVICE} --set-ctrl=tilt_absolute=3600`;
}
