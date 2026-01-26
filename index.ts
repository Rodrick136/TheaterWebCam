import { cc, CString, dlopen, ptr, toBuffer } from "bun:ffi";
import source from "./compile/intercom.c" with { type: "file" };

console.log("Starting Webcam~");


const DEVICE = (await Bun.$`v4l2-ctl --list-devices | grep "Logitech BRIO" -A 1 | tail -n 1 | xargs`.text()).trim();
console.log(`Selected device: ${DEVICE}`)

Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=auto_exposure=1`;
Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=exposure_time_absolute=400`;
Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=gain=0`;
Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=backlight_compensation=0`;
Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=zoom_absolute=300`;
Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=pan_absolute=3600`;
Bun.$`v4l2-ctl -d ${DEVICE} --set-ctrl=tilt_absolute=3600`;

//start the web cam stream
const {
  symbols: { start_cam },
} = dlopen(
    "./compile/libintercom.so",
   {
    start_cam: {
      args: ["cstring"],
      returns: "ptr",
    },
});

const DEVICE_ptr  = ptr(Buffer.from(DEVICE + "\0"));
const result_ptr = start_cam(DEVICE_ptr);
if (result_ptr) {
    const result = new CString(result_ptr).toString();
    console.log(result)
}


process.exit(0)