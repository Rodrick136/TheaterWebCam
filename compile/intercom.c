#include <stdio.h>
#include <signal.h>
#include <gst/gst.h>

static GstElement *g_pipeline = NULL;
static GMainLoop *g_main_loop = NULL;

// Forward declarations
static void configure_audio(void);
static char *setup_display_branch(GstElement *tee, GstElement *display_queue, GstElement *display_sink);
static char *setup_recording_branch(GstElement *tee);

static gboolean is_display_window_closed(const GError *err)
{
    return err && err->message && g_strrstr(err->message, "window was closed") != NULL;
}

static void signal_handler(int signum)
{
    g_print("\nReceived signal %d, cleaning up...\n", signum);

    if (g_main_loop) {
        g_main_loop_quit(g_main_loop);
    }
}

static gboolean bus_callback(GstBus *bus, GstMessage *message, gpointer data)
{
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError *err;
            gchar *debug;
            gst_message_parse_error(message, &err, &debug);
            if (is_display_window_closed(err)) {
                g_print("Display window closed, stopping...\n");
            } else {
                g_printerr("Error: %s\n", err->message);
            }
            g_error_free(err);
            g_free(debug);
            if (g_main_loop) {
                g_main_loop_quit(g_main_loop);
            }
            break;
        }
        case GST_MESSAGE_EOS:
            g_print("End of stream\n");
            if (g_main_loop) {
                g_main_loop_quit(g_main_loop);
            }
            break;
        default:
            break;
    }
    return TRUE;
}

// device_path takes the form of /dev/video0
// should_record is 1 to enable recording, 0 to disable
// video_size of the device to be set, passes /^\d+x\d+$/
char *start_cam(char *device_path, int should_record, char *video_size)
{
    GstElement *source, *convert, *tee, *display_queue, *display_sink;
    GstElement *capsfilter; // capsfilter for width/height negotiation
    GstBus *bus;
    char *error;

    // Initialize GStreamer
    gst_init(NULL, NULL);

    // Create the empty pipeline
    g_pipeline = gst_pipeline_new("webcam-pipeline");

    // Create common elements
    source = gst_element_factory_make("v4l2src", "source");
    capsfilter = gst_element_factory_make("capsfilter", "capsfilter"); // NEW
    convert = gst_element_factory_make("videoconvert", "convert");
    tee = gst_element_factory_make("tee", "tee");
    display_queue = gst_element_factory_make("queue", "display_queue");
    display_sink = gst_element_factory_make("autovideosink", "display_sink");

    if (!g_pipeline || !source || !capsfilter || !convert || !tee || !display_queue || !display_sink) {
        g_printerr("Failed to create basic pipeline elements.\n");
        return "Failed to create GStreamer elements";
    }

    // Set the device property on the source
    g_object_set(source,
        "device", device_path,
        "do-timestamp", TRUE,    // Use pipeline clock for timestamps
        "io-mode", 2,            // Prefer MMAP for lower overhead if supported
        NULL);

    // Enable QoS so transforms can drop late frames
    g_object_set(convert,
        "qos", TRUE,
        NULL);

    // Parse and set video size via capsfilter (not on v4l2src)
    int width, height;
    if (sscanf(video_size, "%dx%d", &width, &height) == 2) {
        GstCaps *caps = gst_caps_new_simple("video/x-raw",
            "width", G_TYPE_INT, width,
            "height", G_TYPE_INT, height,
            NULL);
        g_object_set(capsfilter, "caps", caps, NULL);
        gst_caps_unref(caps);
        g_print("Video size set to: %dx%d\n", width, height);
    } else {
        g_printerr("Invalid video size format: %s\n", video_size);
        return "Invalid video size format. Expected \\d+x\\d+.";
    }

    // Configure tee to not block if one branch is slower
    g_object_set(tee,
        "allow-not-linked", TRUE,    // Don't fail if a branch returns not-linked
        NULL);

    // Add basic elements to pipeline
    gst_bin_add_many(GST_BIN(g_pipeline), source, capsfilter, convert, tee, display_queue, display_sink, NULL);

    // Link: source -> capsfilter -> convert -> tee
    if (!gst_element_link_many(source, capsfilter, convert, tee, NULL)) {
        g_printerr("Failed to link source -> capsfilter -> convert -> tee.\n");
        gst_object_unref(g_pipeline);
        return "Failed to link GStreamer elements";
    }

    // Setup display branch
    error = setup_display_branch(tee, display_queue, display_sink);
    if (error != NULL) {
        gst_object_unref(g_pipeline);
        return error;
    }

    // If recording is enabled, setup recording branch
    if (should_record) {
        error = setup_recording_branch(tee);
        if (error != NULL) {
            gst_object_unref(g_pipeline);
            return error;
        }
    }

    // Add a bus watch
    bus = gst_element_get_bus(g_pipeline);
    gst_bus_add_watch(bus, bus_callback, NULL);
    gst_object_unref(bus);

    // Start playing
    GstStateChangeReturn ret = gst_element_set_state(g_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("Unable to set the pipeline to the playing state.\n");
        gst_object_unref(g_pipeline);
        return "Failed to start pipeline";
    }

    // Setup signal handlers for cleanup
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Create and run the main loop
    g_main_loop = g_main_loop_new(NULL, FALSE);
    g_print("Running webcam stream. Press Ctrl+C to stop.\n");
    g_main_loop_run(g_main_loop);

    // Cleanup - send EOS to properly finalize recording
    g_print("Cleaning up GStreamer pipeline...\n");

    // Send end-of-stream event to finalize files
    gst_element_send_event(g_pipeline, gst_event_new_eos());

    // Wait a bit for EOS to be processed
    GstBus *cleanup_bus = gst_element_get_bus(g_pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(cleanup_bus,
        2 * GST_SECOND,
        GST_MESSAGE_EOS | GST_MESSAGE_ERROR);

    if (msg) {
        gst_message_unref(msg);
    }
    gst_object_unref(cleanup_bus);

    gst_element_set_state(g_pipeline, GST_STATE_NULL);
    gst_object_unref(g_pipeline);
    g_main_loop_unref(g_main_loop);

    return NULL;
}

static char *setup_display_branch(GstElement *tee, GstElement *display_queue, GstElement *display_sink)
{
    GstPad *tee_display_pad, *queue_display_pad;

    // Configure display queue for minimal latency
    g_object_set(display_queue,
        "max-size-buffers", 2,      // Keep only 2 frames buffered for low latency
        "max-size-bytes", 0,         // Disable byte limit
        "max-size-time", 0,          // Disable time limit
        "leaky", 2,                  // Drop oldest buffers when downstream is late
        NULL);

    // Display ASAP without clock sync to minimize latency
    g_object_set(display_sink,
        "sync", FALSE,
        "async", FALSE,      // Don't wait for preroll, prevents blocking when recording
        NULL);

    // Link display branch: tee -> display_queue -> display_sink
    tee_display_pad = gst_element_request_pad_simple(tee, "src_%u");
    queue_display_pad = gst_element_get_static_pad(display_queue, "sink");
    if (gst_pad_link(tee_display_pad, queue_display_pad) != GST_PAD_LINK_OK) {
        g_printerr("Failed to link tee to display queue.\n");
        gst_object_unref(queue_display_pad);
        return "Failed to link display branch";
    }
    gst_object_unref(queue_display_pad);

    if (!gst_element_link(display_queue, display_sink)) {
        g_printerr("Failed to link display queue to sink.\n");
        return "Failed to link display sink";
    }

    return NULL;
}

static char *setup_recording_branch(GstElement *tee)
{
    GstElement *record_queue, *encoder, *video_muxer, *video_file_sink;
    GstPad *tee_record_pad, *queue_record_pad;

    g_print("Recording enabled - video: webcam_video.mp4\n");

    // Create video recording elements
    record_queue = gst_element_factory_make("queue", "record_queue");
    encoder = gst_element_factory_make("x264enc", "encoder");
    video_muxer = gst_element_factory_make("mp4mux", "video_muxer");
    video_file_sink = gst_element_factory_make("filesink", "video_file_sink");

    if (!record_queue || !encoder || !video_muxer || !video_file_sink) {
        g_printerr("Failed to create video recording elements.\n");
        return "Failed to create video recording elements";
    }

    // Configure recording queue - leaky to prevent blocking
    g_object_set(record_queue,
        "max-size-buffers", 200,
        "max-size-bytes", 0,
        "max-size-time", 0,
        "leaky", 2,
        NULL);

    // Configure encoder for AVCC + low-latency
    g_object_set(encoder,
        "byte-stream", FALSE,       // AVCC format required by mp4mux
        "speed-preset", 1,          // ultrafast
        "bitrate", 2048,            // ~2 Mbps
        "key-int-max", 30,          // frequent IDR
        "bframes", 0,               // no reordering
        "rc-lookahead", 0,          // no lookahead
        NULL);
    // Set tune=zerolatency if available (parse via util to avoid enum mismatch)
    {
        GParamSpec *ps = g_object_class_find_property(G_OBJECT_GET_CLASS(encoder), "tune");
        if (ps) {
            gst_util_set_object_arg(G_OBJECT(encoder), "tune", "zerolatency");
        }
    }

    // Configure video filesink
    g_object_set(video_file_sink,
        "location", "webcam_video.mp4",
        "async", FALSE,
        NULL);

    // Optional: mp4mux faststart if supported
    {
        GParamSpec *ps = g_object_class_find_property(G_OBJECT_GET_CLASS(video_muxer), "faststart");
        if (ps) {
            g_object_set(video_muxer, "faststart", TRUE, NULL);
        }
    }

    // Add video recording elements to pipeline
    gst_bin_add_many(GST_BIN(g_pipeline), record_queue, encoder, video_muxer, video_file_sink, NULL);

    // Link video recording: tee -> record_queue -> encoder -> video_muxer -> video_file_sink
    tee_record_pad = gst_element_request_pad_simple(tee, "src_%u");
    queue_record_pad = gst_element_get_static_pad(record_queue, "sink");
    if (gst_pad_link(tee_record_pad, queue_record_pad) != GST_PAD_LINK_OK) {
        g_printerr("Failed to link tee to record queue.\n");
        gst_object_unref(queue_record_pad);
        return "Failed to link record branch";
    }
    gst_object_unref(queue_record_pad);

    if (!gst_element_link_many(record_queue, encoder, video_muxer, video_file_sink, NULL)) {
        g_printerr("Failed to link video recording pipeline.\n");
        return "Failed to link video recording pipeline";
    }

    // Configure and attach audio recording branch (non-fatal on failure)
    configure_audio();

    return NULL;
}

static void configure_audio(void)
{
    GstElement *voice_src, *voice_convert, *voice_resample, *voice_queue, *voice_encoder, *voice_file_sink;
    GstElement *fx_src, *fx_convert, *fx_resample, *fx_queue, *fx_encoder, *fx_file_sink;

    // Create Voice input: prefer PipeWire, fallback to PulseAudio
    voice_src = gst_element_factory_make("pipewiresrc", "voice_src");
    if (!voice_src) {
        voice_src = gst_element_factory_make("pulsesrc", "voice_src");
    }
    

    // Create Effects input: prefer PipeWire, fallback to PulseAudio
    fx_src = gst_element_factory_make("pipewiresrc", "fx_src");
    if (!fx_src) {
        fx_src = gst_element_factory_make("pulsesrc", "fx_src");
    }
    

    if (!voice_src && !fx_src) {
        g_printerr("No audio sources available; audio disabled.\n");
        return;
    }

    // Voice branch
    if (voice_src) {
        voice_convert = gst_element_factory_make("audioconvert", "voice_convert");
        voice_resample = gst_element_factory_make("audioresample", "voice_resample");
        voice_queue = gst_element_factory_make("queue", "voice_audio_queue");
        voice_encoder = gst_element_factory_make("lamemp3enc", "voice_audio_encoder");
        voice_file_sink = gst_element_factory_make("filesink", "voice_audio_file_sink");

        if (!voice_convert || !voice_resample || !voice_queue || !voice_encoder || !voice_file_sink) {
            g_printerr("Failed to create voice audio elements. Voice audio disabled.\n");
        } else {
            // Set common properties, guarding for availability
            g_object_set(voice_src,
                "do-timestamp", TRUE,
                NULL);
            GParamSpec *ps;
            ps = g_object_class_find_property(G_OBJECT_GET_CLASS(voice_src), "client-name");
            if (ps) {
                g_object_set(voice_src, "client-name", "Voice In", NULL);
            }
            ps = g_object_class_find_property(G_OBJECT_GET_CLASS(voice_src), "provide-clock");
            if (ps) {
                g_object_set(voice_src, "provide-clock", FALSE, NULL);
            }
            ps = g_object_class_find_property(G_OBJECT_GET_CLASS(voice_src), "buffer-time");
            if (ps) {
                g_object_set(voice_src, "buffer-time", (gint64)200000, NULL);
            }

            g_object_set(voice_queue,
                "max-size-buffers", 200,
                "leaky", 2,
                NULL);

            g_object_set(voice_file_sink,
                "location", "webcam_voice.mp3",
                "async", FALSE,
                NULL);

            gst_bin_add_many(GST_BIN(g_pipeline), voice_src, voice_convert,
                             voice_resample, voice_queue, voice_encoder, voice_file_sink, NULL);

            if (!gst_element_link_many(voice_src, voice_convert, voice_resample,
                                      voice_queue, voice_encoder, voice_file_sink, NULL)) {
                g_printerr("Failed to link voice audio pipeline. Voice audio disabled.\n");
            } else {
                g_print("Voice audio input exposed (patch via qpwgraph).\n");
            }
        }
    }

    // Effects branch
    if (fx_src) {
        fx_convert = gst_element_factory_make("audioconvert", "fx_convert");
        fx_resample = gst_element_factory_make("audioresample", "fx_resample");
        fx_queue = gst_element_factory_make("queue", "fx_audio_queue");
        fx_encoder = gst_element_factory_make("lamemp3enc", "fx_audio_encoder");
        fx_file_sink = gst_element_factory_make("filesink", "fx_audio_file_sink");

        if (!fx_convert || !fx_resample || !fx_queue || !fx_encoder || !fx_file_sink) {
            g_printerr("Failed to create effects audio elements. Effects audio disabled.\n");
        } else {
            // Set common properties, guarding for availability
            g_object_set(fx_src,
                "do-timestamp", TRUE,
                NULL);
            GParamSpec *ps2;
            ps2 = g_object_class_find_property(G_OBJECT_GET_CLASS(fx_src), "client-name");
            if (ps2) {
                g_object_set(fx_src, "client-name", "Effects In", NULL);
            }
            ps2 = g_object_class_find_property(G_OBJECT_GET_CLASS(fx_src), "provide-clock");
            if (ps2) {
                g_object_set(fx_src, "provide-clock", FALSE, NULL);
            }
            ps2 = g_object_class_find_property(G_OBJECT_GET_CLASS(fx_src), "buffer-time");
            if (ps2) {
                g_object_set(fx_src, "buffer-time", (gint64)200000, NULL);
            }

            g_object_set(fx_queue,
                "max-size-buffers", 200,
                "leaky", 2,
                NULL);

            g_object_set(fx_file_sink,
                "location", "webcam_effects.mp3",
                "async", FALSE,
                NULL);

            gst_bin_add_many(GST_BIN(g_pipeline), fx_src, fx_convert,
                             fx_resample, fx_queue, fx_encoder, fx_file_sink, NULL);

            if (!gst_element_link_many(fx_src, fx_convert, fx_resample,
                                      fx_queue, fx_encoder, fx_file_sink, NULL)) {
                g_printerr("Failed to link effects audio pipeline. Effects audio disabled.\n");
            } else {
                g_print("Effects audio input exposed (patch via qpwgraph).\n");
            }
        }
    }
}
