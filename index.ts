import { mkdirSync } from "node:fs";
import { parseArgs } from "node:util";

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

const DEVICE = (
  await Bun.$`v4l2-ctl --list-devices | grep "Logitech BRIO" -A 1 | tail -n 1 | xargs`.text()
).trim();
console.log(`Selected cam device: ${DEVICE}`);
console.log(`Video size: ${VIDEO_SIZE}`);
console.log(`Framerate: ${FRAMERATE}`);
//configure the web cam settings
//Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=white_balance_automatic=0`;
//Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=white_balance_temperature=4500`;

await Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=auto_exposure=1`;
await Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=exposure_time_absolute=400`;
await Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=gain=0`;
await Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=backlight_compensation=0`;
await Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=zoom_absolute=300`;

if (FOCUS !== undefined) {
  console.log(`Focus set to: ${FOCUS}`);
  await Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=focus_automatic_continuous=0`;
  await Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=focus_absolute=${FOCUS}`;
}
//Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=pan_absolute=3600`;
//Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=tilt_absolute=3600`;

// Recording folder name
const DIR_NAME = `Recording__${new Date().toISOString()}`;
// Create recording directory if recording is enabled
if (RECORD) {
  mkdirSync(DIR_NAME);
}

// start recording audio here
let _voiceProc: Bun.Subprocess | null = null;
let _effectsProc: Bun.Subprocess | null = null;
let _videoProc: Bun.Subprocess<"pipe", "inherit", "inherit"> | null = null;
if (RECORD) {
  const voicePath = `${DIR_NAME}/webcam_voice.wav`;
  const effectsPath = `${DIR_NAME}/webcam_effects.wav`;

  // spawn pw-record processes (run until terminated)
  try {
    _voiceProc = Bun.spawn({
      cmd: [
        "pw-record",
        "--channels=1",
        "--rate=48000",
        "--properties",
        `media.name=webcam_voice,node.name=webcam_voice,application.name=TheaterWebCam-Voice`,
        voicePath,
      ],
      stdout: "inherit",
      stdin: "pipe",
      stderr: "inherit",
    });
  } catch (e) {
    console.error("Failed to start voice recorder:", e);
  }

  try {
    _effectsProc = Bun.spawn({
      cmd: [
        "pw-record",
        "--channels=2",
        "--rate=48000",
        "--properties",
        `media.name=webcam_effects,node.name=webcam_effects,application.name=TheaterWebCam-Effects`,
        effectsPath,
      ],
      stdout: "inherit",
      stdin: "pipe",
      stderr: "inherit",
    });
  } catch (e) {
    console.error("Failed to start effects recorder:", e);
  }

  try {
    _videoProc = Bun.spawn({
      cmd: [
        "gst-launch-1.0",
        "-v",
        "v4l2src",
        `device=${DEVICE}`,
        "do-timestamp=true",
        "io-mode=2",
        "!",
        "image/jpeg,",
        `width=${WIDTH},`,
        `height=${HEIGHT},`,
        `framerate=${FRAMERATE}/1`,
        "!",
        "jpegdec",
        "!",
        "videoconvert",
        "qos=true",
        "!",
        "tee",
        "name=t",
        // video preview branch
        "t.",
        "!",
        "queue",
        "max-size-buffers=2",
        "max-size-bytes=0",
        "max-size-time=0",
        "leaky=2",
        "!",
        "autovideosink",
        "sync=false",
        // recording branch
        "t.",
        "!",
        "queue",
        "max-size-buffers=600",
        "max-size-bytes=0",
        "max-size-time=0",
        "leaky=2",
        "!",
        "x264enc",
        "speed-preset=6",
        "bitrate=8192",
        "key-int-max=60",
        "qp-min=10",
        //"tune=film",
        "!",
        "queue",
        "max-size-buffers=300",
        "leaky=2",
        "!",
        "mp4mux",
        "faststart=true",
        "!",
        "filesink",
        `location=./${DIR_NAME}/webcam_video.mp4`,
        "async=false",
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
        "-v",
        "v4l2src",
        `device=${DEVICE}`,
        "do-timestamp=true",
        "io-mode=2",
        "!",
        "image/jpeg,",
        `width=${WIDTH},`,
        `height=${HEIGHT},`,
        `framerate=${FRAMERATE}/1`,
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
        "leaky=2",
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

// Helper: wait for a promise to resolve within `ms` milliseconds
const waitFor = async (p: Promise<number>, ms: number) => {
  try {
    return await Promise.race([p.then(() => true), new Promise<boolean>((res) => setTimeout(() => res(false), ms))]);
  } catch (e) {
    return true; // treat rejection as finished
  }
};

// Kill a subprocess gracefully: send SIGINT, wait, then SIGTERM if still alive
const killAndAwait = async (proc: Bun.Subprocess | null) => {
  if (!proc) return;
  proc.kill("SIGINT");
  const finished = await waitFor(proc.exited, 3000);
  if (!finished) {
    proc.kill("SIGTERM");
    // give it a bit more time to terminate
    await waitFor(proc.exited, 2000);
  }
};

const exit = async () => {
  await Promise.all([killAndAwait(_videoProc), killAndAwait(_voiceProc), killAndAwait(_effectsProc)]);
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