import { cc, CString, dlopen, ptr, toBuffer } from "bun:ffi";
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

Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=auto_exposure=1`;
Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=exposure_time_absolute=400`;
Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=gain=0`;
Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=backlight_compensation=0`;
Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=zoom_absolute=300`;

if (FOCUS !== undefined) {
  console.log(`Focus set to: ${FOCUS}`);
  Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=focus_automatic_continuous=0`;
  Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=focus_absolute=${FOCUS}`;
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
      stdin: "inherit",
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
      stdin: "inherit",
      stderr: "inherit",
    });
  } catch (e) {
    console.error("Failed to start effects recorder:", e);
  }
}

//start the web cam stream
const {
  symbols: { start_cam },
} = dlopen("./compile/libintercom.so", {
  start_cam: {
    args: ["cstring", "cstring", "bool", "cstring", "cstring"],
    returns: "ptr",
  },
});

const DIR_NAME_ptr = ptr(Buffer.from(DIR_NAME + "\0"));
const DEVICE_ptr = ptr(Buffer.from(DEVICE + "\0"));
const VIDEO_SIZE_ptr = ptr(Buffer.from(VIDEO_SIZE + "\0"));
const FRAMERATE_ptr = ptr(Buffer.from(FRAMERATE + "\0"));
const result_ptr = start_cam(
  DIR_NAME_ptr,
  DEVICE_ptr,
  RECORD,
  VIDEO_SIZE_ptr,
  FRAMERATE_ptr,
);
if (result_ptr) {
  const result = new CString(result_ptr).toString();
  console.error("Error:", result);
  process.exit(1);
}

// When start_cam returns, stop recorders and await exit
async function stopRecorders() {
  const procs: Array<Promise<number>> = [];
  if (_voiceProc) {
    try {
      _voiceProc.kill("SIGTERM");
    } catch {}
    if (_voiceProc.exited) procs.push(_voiceProc.exited);
  }
  if (_effectsProc) {
    try {
      _effectsProc.kill("SIGTERM");
    } catch {}
    if (_effectsProc.exited) procs.push(_effectsProc.exited);
  }
  await Promise.all(procs);
}

await stopRecorders();
