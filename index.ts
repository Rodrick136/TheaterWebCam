import { cc, CString, dlopen, ptr, toBuffer } from "bun:ffi";
import { parseArgs } from "util";

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

//start the web cam stream
const {
  symbols: { start_cam },
} = dlopen("./compile/libintercom.so", {
  start_cam: {
    args: ["cstring", "bool", "cstring", "cstring"],
    returns: "ptr",
  },
});

const DEVICE_ptr = ptr(Buffer.from(DEVICE + "\0"));
const VIDEO_SIZE_ptr = ptr(Buffer.from(VIDEO_SIZE + "\0"));
const FRAMERATE_ptr = ptr(Buffer.from(FRAMERATE + "\0"));
const result_ptr = start_cam(DEVICE_ptr, RECORD, VIDEO_SIZE_ptr, FRAMERATE_ptr);
if (result_ptr) {
  const result = new CString(result_ptr).toString();
  console.error("Error:", result);
  process.exit(1);
}

// find latest recording directory
/* if (RECORD) {
  const RECORDING_DIR = (
    await Bun.$`/usr/bin/ls -td ./Recording__* | head -n 1`.text()
  ).trim();
  console.log(`Latest recording saved in: ${RECORDING_DIR}`);

  // use ffmpeg to combine video and audio
  console.log("Combining video and audio into final output file...");
  const VIDEO = RECORDING_DIR + "/webcam_video.mp4"; // no audio yet
  const VOICE = RECORDING_DIR + "/webcam_voice.mp3"; // 2 channels
  const EFFECTS = RECORDING_DIR + "/webcam_effects.mp3"; // 2 channels

  const OUTPUT = RECORDING_DIR + "/final_output.mp4";
  await Bun.$`ffmpeg -i ${VIDEO} -i ${VOICE} -i ${EFFECTS} -filter_complex "[1:a][2:a]amerge=inputs=2[aout]" -map 0:v -map "[aout]" -c:v copy -ac 4 -c:a aac ${OUTPUT} -y`;
  console.log(`Final output saved in: ${OUTPUT}`);
} */
