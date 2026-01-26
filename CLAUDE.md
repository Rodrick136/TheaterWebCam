# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**Linux-only Bun + TypeScript + C hybrid application** for professional webcam streaming targeting theater/stage productions with a **Logitech BRIO webcam**. The app uses Bun FFI to call native C code that orchestrates GStreamer pipelines for low-latency display and optional recording.

## Core Architecture

### Component Interaction Flow
1. [index.ts](index.ts) - TypeScript entry point
   - Parses CLI arguments (`--record`, `--focus`, `--video-size`)
   - Detects Logitech BRIO device via v4l2-ctl
   - Configures theater-optimized camera settings via v4l2-ctl
   - Loads C library via Bun FFI and invokes `start_cam()`

2. [compile/intercom.c](compile/intercom.c) - C orchestration layer
   - Creates GStreamer pipeline with tee element for dual video paths
   - **Display branch**: minimal latency (2 buffers, no sync, leaky queue)
   - **Recording branch**: x264enc → MP4 (200 buffers, leaky queue)
   - **Audio branches**: separate voice and effects pipelines (pipewiresrc/pulsesrc → MP3)
   - Handles graceful shutdown with EOS event on SIGINT/SIGTERM

### Critical Design Patterns

**Bun FFI String Passing**
C functions receive null-terminated string buffers via `ptr(Buffer.from(str + "\0"))`. The C function returns `NULL` on success or an error string pointer on failure.

**GStreamer Tee Architecture**
Video splits at tee element into independent branches. Display branch prioritizes low latency (sync=FALSE, 2 buffers). Recording branch uses leaky queue (200 buffers) to never block display.

**Audio Pipeline Independence**
When recording is enabled, two separate audio pipelines run parallel to video:
- Voice: pipewiresrc/pulsesrc → webcam_voice.mp3
- Effects: pipewiresrc/pulsesrc → webcam_effects.mp3
These expose as PipeWire/PulseAudio inputs named "Voice In" and "Effects In" for external patching via qpwgraph.

**Hardware Configuration Constraints**
v4l2 settings in [index.ts](index.ts) are theater-optimized and should not be changed without testing:
- Manual exposure (auto_exposure=1, exposure_time_absolute=400)
- Zero gain, no backlight compensation
- 3x digital zoom (zoom_absolute=300)
- Manual focus (focus_automatic_continuous=0)

## Development Commands

### Build C Library
```bash
bun run compile
```
Compiles [compile/intercom.c](compile/intercom.c) into `libintercom.so` using gcc with GStreamer 1.0 dependencies. **Must be run after any C code changes.**

### Run Application
```bash
# Display only
bun run index.ts

# With recording (creates webcam_video.mp4, webcam_voice.mp3, webcam_effects.mp3)
bun run index.ts --record

# Adjust focus (0-255, default: 50)
bun run index.ts --focus 75

# Set video resolution (default: 1280x720)
bun run index.ts --video-size 1920x1080
```

The application blocks until Ctrl+C, which triggers signal handler cleanup and proper file finalization via EOS event.

## System Dependencies

- **Linux OS** - Required for v4l2 API
- **Bun runtime** - JavaScript runtime with FFI
- **GStreamer 1.0** - Installed via system package manager
- **v4l2-utils** - For v4l2-ctl command
- **Logitech BRIO webcam** - Only supported device

## Important Implementation Notes

- Recording produces **separate video and audio files**, not muxed output
- The C `start_cam()` function blocks the main thread until Ctrl+C - no programmatic stop mechanism
- Display uses `autovideosink` which auto-selects best backend (wayland/X11)
- Video size is enforced via capsfilter after v4l2src, not on the source element directly
- Device detection greps for "Logitech BRIO" string in v4l2-ctl output
- Audio inputs appear as separate PipeWire/PulseAudio clients that must be patched externally to physical inputs
