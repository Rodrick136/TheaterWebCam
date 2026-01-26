# TheaterWebCam Project Instructions

## Architecture Overview

This is a **Linux-only Bun + TypeScript + C hybrid application** for professional webcam streaming targeting theater/stage productions with a **Logitech BRIO webcam**. The app uses:
- **Bun FFI** to call native C code that orchestrates GStreamer pipelines
- **v4l2** for hardware-level webcam control (Linux Video4Linux2 API)
- **GStreamer** (in C) for dual-path video: low-latency display + optional recording

### Key Components
- [index.ts](../index.ts): TypeScript entry point that parses CLI args, configures v4l2 camera settings, and invokes the C FFI
- [compile/intercom.c](../compile/intercom.c): C code using GStreamer to create a tee pipeline with separate display and recording branches
- [ref_webcam_hdmi.sh](../ref_webcam_hdmi.sh): Reference bash implementation using ffmpeg (not actively used)

## Critical Workflows

### Building the C Library
The C code must be compiled before running. Use:
```bash
bun run compile
```
This compiles `intercom.c` into `libintercom.so` using gcc with GStreamer 1.0 dependencies.

### Running the Application
```bash
# Basic streaming (display only)
bun run index.ts

# With recording (creates webcam_video.mp4 and webcam_audio.mp3)
bun run index.ts --record

# Adjust focus (default: 50)
bun run index.ts --focus 75

# Set video resolution (default: 1280x720)
bun run index.ts --video-size 1920x1080
```

## Project-Specific Patterns

### Bun FFI Bridge Pattern
The TypeScript code loads the compiled C library using Bun's FFI:
```typescript
const { symbols: { start_cam } } = dlopen("./compile/libintercom.so", {
  start_cam: { args: ["cstring", "bool", "cstring"], returns: "ptr" }
});
```
Strings are passed as null-terminated buffers using `ptr(Buffer.from(str + "\0"))`. The C function returns `NULL` on success or an error string pointer on failure.

### Hardware Configuration
v4l2-ctl commands in [index.ts](../index.ts) set theater-optimized Logitech BRIO controls before streaming:
- Manual exposure (auto_exposure=1, exposure_time_absolute=400)
- Zero gain, no backlight compensation
- 3x digital zoom (zoom_absolute=300)
- Manual focus (focus_automatic_continuous=0)

These values are specifically tuned for theater/stage lighting and should not be changed without testing. Device detection greps for "Logitech BRIO" in v4l2-ctl output.

### GStreamer Pipeline Architecture
The C code creates a **tee element** that splits video into two independent branches:
1. **Display branch**: `tee -> display_queue (2 buffers) -> autovideosink` for minimal latency
2. **Recording branch**: `tee -> record_queue (200 buffers, leaky) -> x264enc -> mp4mux -> filesink`

Audio recording (when enabled) runs as a separate parallel pipeline: `pulsesrc -> audioconvert -> audioresample -> lamemp3enc -> filesink (MP3)`

### Cleanup Pattern
The C code uses signal handlers (SIGINT/SIGTERM) to gracefully shutdown. On exit:
1. Sends EOS (end-of-stream) event to finalize recording files
2. Waits up to 2 seconds for EOS processing
3. Sets pipeline to NULL state and unrefs all GStreamer objects

## Dependencies

- **Linux OS**: Required for v4l2 webcam control
- **Bun runtime**: JavaScript runtime with native FFI support
- **GStreamer 1.0**: System package required for C compilation and runtime
- **v4l2-utils**: Camera device control via Video4Linux2 API
- **Logitech BRIO webcam**: The only supported device

## Implementation Notes

- Recording produces separate video (MP4/H.264) and audio (MP3) files, not a single muxed file
- The C `start_cam` function blocks until Ctrl+C - no programmatic stop mechanism exists
- Display uses `autovideosink` which auto-selects the best backend for the Linux system
