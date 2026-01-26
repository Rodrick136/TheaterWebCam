import { cc, CString, dlopen, ptr, toBuffer } from "bun:ffi";
import { parseArgs } from "util";

// Parse command-line arguments
const { values, positionals } = parseArgs({
  args: Bun.argv,
  options: {
    record: {
      type: "boolean",
      short: "r",
      default: false
    },
    focus: {
      type: "string",
      short: "f",
      default: "50"
    },
    "video-size": {
      type: "string",
      short: "v",
      default: "1280x720"
    },
  },
  strict: true,
  allowPositionals: true,
});
const RECORD = values.record;

const FOCUS = Number.parseInt(values.focus);
if (Number.isNaN(FOCUS)) {
  console.log("Arg --focus has to be an integer");
  process.exit(1);
}

const VIDEO_SIZE = values["video-size"];
const vs_regex = /^\d+x\d+$/
if (vs_regex.test(VIDEO_SIZE) === false) {
  console.log("Arg --video-size has to meet pattern /^\\d+x\\d+$/");
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

Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=auto_exposure=1`;
Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=exposure_time_absolute=400`;
Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=gain=0`;
Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=backlight_compensation=0`;
Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=zoom_absolute=300`;
Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=focus_automatic_continuous=0`;
Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=focus_absolute=${FOCUS}`;
//Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=pan_absolute=3600`;
//Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=tilt_absolute=3600`;

//start the web cam stream
const {
  symbols: { start_cam },
} = dlopen("./compile/libintercom.so", {
  start_cam: {
    args: ["cstring", "bool", "cstring"],
    returns: "ptr",
  },
});

const DEVICE_ptr = ptr(Buffer.from(DEVICE + "\0"));
const VIDEO_SIZE_ptr = ptr(Buffer.from(VIDEO_SIZE + "\0"));
const result_ptr = start_cam(DEVICE_ptr, RECORD, VIDEO_SIZE_ptr);
if (result_ptr) {
  const result = new CString(result_ptr).toString();
  console.error("Error:", result);
  process.exit(1);
}

// start_cam blocks until Ctrl+C or an error occurs
// The C code handles cleanup on exit
