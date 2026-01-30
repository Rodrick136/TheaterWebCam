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
    help: {
      type: "boolean",
      short: "h",
      default: false,
    },
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
    "effects-device-l": {
      type: "string",
    },
    "effects-device-r": {
      type: "string",
    },
    "voice-device": {
      type: "string",
    },
    "hardware-acceleration": {
      type: "boolean",
      default: false,
    },
  },
  strict: true,
  allowPositionals: true,
});
if (values.help) {
  console.log(`
Usage: bun index.ts [options]

Options:
  -h, --help                          Show this help message
  -r, --record                        Enable recording mode
  -f, --focus <value>                 Set focus value (integer)
      --video-size <WxH>              Set video size (default: 1920x1080)
      --framerate <30|60>             Set framerate (default: 30)
      --effects-device-l <dev>:<port> Link left effects audio device port
      --effects-device-r <dev>:<port> Link right effects audio device port
      --voice-device <dev>:<port>     Link voice audio device port
      --hardware-acceleration         Enable hardware acceleration (VAAPI) - WIP

Example:
  bun index.ts --record --focus 20 --video-size 1280x720 --framerate 60
`);
  process.exit(0);
}


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

const HARDWARE_ACCELERATION = values["hardware-acceleration"];

console.log("Starting Webcam~");
if (RECORD) {
  console.log("Recording mode enabled");
}

const v4l2_DEVICE = (
  await Bun.$`v4l2-ctl --list-devices | grep "Logitech BRIO" -A 1 | tail -n 1 | xargs`.text()
).trim();
const devices = JSON.parse((await Bun.$`pw-dump`.text()).trim());
const webcams = [];
const default_sources = [];
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
    if (props["media.class"] === "Audio/Source") {
      //console.log(device);
      default_sources.push({
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

const hasVaapi = async () => {
  if (HARDWARE_ACCELERATION === false) {
    return false;
  }
  try {
    const p = Bun.spawn({
      cmd: ["gst-inspect-1.0", "vaapijpegdec"],
      stdout: "pipe",
      stdin: "pipe",
      stderr: "pipe",
    });
    await p.exited;
    const text = await p.stdout.text();

    return text.startsWith("No such element or plugin") === false;
  } catch {
    return false;
  }
};

const vaapiAvailable = await hasVaapi();
if (vaapiAvailable) {
  console.log("Pipeline: using VAAPI hardware acceleration for display");
} else {
  console.log("Pipeline: using CPU pipeline");
}

let _videoProc: Bun.Subprocess<"pipe", "inherit", "inherit"> | null = null;
//let _audioProc: Bun.Subprocess<"pipe", "inherit", "inherit"> | null = null;
let audioModule: number | null = null;
if (RECORD) {
  try {
    // create a virtual audio sink with 3 channels
    const sink_name = "TheaterWebcam-AUDIO_SINK";
    {
      console.log("use pw-link -lm to list sources and sinks as their connections are made");
      const json_string = (
        await Bun.$`pactl -f json load-module module-null-sink sink_name=${sink_name} channels=3 channel_map=mono,left,right`.text()
      ).trim();
      const json = JSON.parse(json_string);
      audioModule = json.index;
      console.log(`Created virtual audio sink with module ID:`, sink_name, audioModule);

      // connect sink_name to default capture source
      if (values["effects-device-l"] && values["effects-device-r"]) {
        await Bun.$`pw-link ${values["effects-device-l"]} ${sink_name}:playback_FL`;
        await Bun.$`pw-link ${values["effects-device-r"]} ${sink_name}:playback_FR`;
        console.log(`Connected effects devices to virtual audio sink`);
      } else if (default_sources[0]) {
        const default_source = default_sources[0];
        await Bun.$`pw-link ${default_source.node_name}:capture_FL ${sink_name}:playback_FL`;
        await Bun.$`pw-link ${default_source.node_name}:capture_FR ${sink_name}:playback_FR`;
        console.log(`Connected default capture to virtual audio sink`);
      }
      if (values["voice-device"]) {
        await Bun.$`pw-link ${values["voice-device"]} ${sink_name}:playback_M`;
        console.log(`Connected voice device to virtual audio sink`);
      }

    }

    // prettier-ignore
    const vaapiCmd = [
      "gst-launch-1.0", "-v", "-e",
      "v4l2src", `device=${v4l2_DEVICE}`, "!",
      `image/jpeg,width=${WIDTH},height=${HEIGHT},framerate=${FRAMERATE}/1`, "!",
      "queue", "leaky=downstream", "max-size-buffers=2", "!",
      "jpegparse", "!", 
      "vaapijpegdec", "!",
      "vaapipostproc", "!",
      "tee", "name=t", 
      // Display branch for video
      "t.", "!",
      "queue", "leaky=downstream", "!",
      "vaapisink", "sync=false",
      // Second display branch (opens another window)
      "t.", "!",
      "queue", "leaky=downstream", "!",
      "vaapisink", "sync=false",
      // Recording branch for video
      "t.", "!",
      "queue", "max-size-buffers=300", "!",

      "x264enc", "speed-preset=ultrafast", "tune=zerolatency", "bitrate=8192", "!",
      "h264parse", "!",
      "mux.video_0",

      "pulsesrc", `client-name=TheaterWebcam-AUDIO`, `device=${sink_name}.monitor`, "!",
      "audio/x-raw,channels=3", "!",
      "queue", "leaky=downstream", "max-size-time=3000000000", "max-size-buffers=0", "!",
      "audioconvert", "!", 
      "audioresample", "!",
      "mux.audio_0",

      "matroskamux", "name=mux", "!",
      "filesink", `location=./${DIR_NAME}/webcam_full.mkv`,
    ];

    // prettier-ignore
    const cpuCmd = [
      "gst-launch-1.0", "-v", "-e",
      "v4l2src", `device=${v4l2_DEVICE}`, "!",
      `image/jpeg,width=${WIDTH},height=${HEIGHT},framerate=${FRAMERATE}/1`, "!",
      "queue", "leaky=downstream", "max-size-buffers=2", "!",
      "jpegdec", "!",
      "tee", "name=t", "t.", "!",
      "queue", "leaky=downstream", "!",
      "autovideosink", "sync=false",
      // Second display branch (opens another window)
      "t.", "!",
      "queue", "leaky=downstream", "!",
      "autovideosink", "sync=false",
      // Recording branch for video
      "t.", "!",
      "queue", "max-size-buffers=300", "!",
      "x264enc", "speed-preset=ultrafast", "tune=zerolatency", "bitrate=8192", "!",
      "h264parse", "!",
      "mux.video_0",

      "pulsesrc", `client-name=TheaterWebcam-AUDIO`, `device=${sink_name}.monitor`, "!",
      "audio/x-raw,channels=3", "!",
      "queue", "leaky=downstream", "max-size-time=3000000000", "max-size-buffers=0", "!",
      "audioconvert", "!", 
      "audioresample", "!",
      "mux.audio_0",

      "matroskamux", "name=mux", "!",
      "filesink", `location=./${DIR_NAME}/webcam_full.mkv`,
    ];

    _videoProc = Bun.spawn({
      cmd: vaapiAvailable ? vaapiCmd : cpuCmd,
      stdout: "inherit",
      stdin: "pipe",
      stderr: "inherit",
    });
  } catch (e) {
    console.error("Failed to start video recorder:", e);
  }

  /* 
  try {
    // Audio subprocess: capture voice into an MP3 file (replace matroskamux)
    // This encodes audio to MP3 directly and writes audio.mp3.
    _audioProc = Bun.spawn({
      // prettier-ignore
      cmd: [
        "pw-record",
        "--target=0",
        "--media-category=Capture",
        "--properties=media.name=AUDIO",
        "--channels=3",
        "--rate=48000",
        `./${DIR_NAME}/audio.wav`
      ],
      stdout: "inherit",
      stdin: "pipe",
      stderr: "inherit",
    });
  } catch (e) {
    console.error("Failed to start audio recorder:", e);
  } */
} else {
  // just play the video without recording
  try {
    const vaapiViewCmd = [
      "gst-launch-1.0",
      "-v",
      "-e",
      "v4l2src",
      `device=${v4l2_DEVICE}`,
      "!",
      `image/jpeg,width=${WIDTH},height=${HEIGHT},framerate=${FRAMERATE}/1`,
      "!",
      "queue",
      "leaky=downstream",
      "max-size-buffers=2",
      "!",
      "jpegparse",
      "!",
      "vaapijpegdec",
      "!",
      "vaapipostproc",
      "!",
      // split to two display branches
      "tee",
      "name=t",
      "t.",
      "!",
      "queue",
      "leaky=downstream",
      "!",
      "vaapisink",
      "sync=false",
      "t.",
      "!",
      "queue",
      "leaky=downstream",
      "!",
      "autovideosink",
      "sync=false",
    ];

    const cpuViewCmd = [
      "gst-launch-1.0",
      "v4l2src",
      `device=${v4l2_DEVICE}`,
      "!",
      `image/jpeg,width=${WIDTH},height=${HEIGHT},framerate=${FRAMERATE}/1`,
      "!",
      "jpegdec",
      "!",
      // split to two display branches
      "tee",
      "name=t",
      "t.",
      "!",
      "queue",
      "max-size-buffers=2",
      "max-size-bytes=0",
      "max-size-time=0",
      "leaky=downstream",
      "!",
      "autovideosink",
      "sync=false",
      "t.",
      "!",
      "queue",
      "max-size-buffers=2",
      "max-size-bytes=0",
      "max-size-time=0",
      "leaky=downstream",
      "!",
      "autovideosink",
      "sync=false",
    ];

    _videoProc = Bun.spawn({
      cmd: vaapiAvailable ? vaapiViewCmd : cpuViewCmd,
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
    try {
      console.log("Stopping video process...");
      _videoProc.kill("SIGINT");
      await _videoProc.exited;
      console.log("Video process stopped.");
    } catch (e) {
      console.warn("Error stopping video process:", e);
    }
  }
  // cleanup the virtual audio sink
  if (audioModule) {
    try {
      console.log(`Unloading virtual audio sink:"${audioModule}"`);
      await Bun.$`pactl unload-module ${audioModule}`;
      console.log("Virtual audio sink unloaded.");
    } catch (e) {
      console.warn("Error unloading virtual audio sink:", e);
    }
  }
  /* if (_audioProc) {
    try {
      console.log("Stopping audio process...");
      _audioProc.kill("SIGINT");
      await _audioProc.exited;
      console.log("Audio process stopped.");
    } catch (e) {
      console.warn("Error stopping audio process:", e);
    }
  } */
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
