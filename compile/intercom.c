#include <stdio.h>
#include <signal.h>
#include <gst/gst.h>

static GstElement *g_pipeline = NULL;
static GMainLoop *g_main_loop = NULL;

static void signal_handler(int signum)
{
    g_print("Received signal %d, cleaning up...\n", signum);

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
            g_printerr("Error: %s\n", err->message);
            g_error_free(err);
            g_free(debug);
            g_main_loop_quit(g_main_loop);
            break;
        }
        case GST_MESSAGE_EOS:
            g_print("End of stream\n");
            g_main_loop_quit(g_main_loop);
            break;
        default:
            break;
    }
    return TRUE;
}

// device_path takes the form of /dev/video0
// should_record is 1 to enable recording, 0 to disable
char *start_cam(char *device_path, int should_record)
{
    GstElement *source, *convert, *tee, *display_queue, *record_queue;
    GstElement *display_sink, *encoder, *video_muxer, *video_file_sink;
    GstElement *audio_src, *audio_convert, *audio_resample, *audio_queue, *audio_encoder, *audio_file_sink;
    GstBus *bus;
    GstPad *tee_display_pad, *tee_record_pad;
    GstPad *queue_display_pad, *queue_record_pad;

    // Initialize GStreamer
    gst_init(NULL, NULL);

    // Create the empty pipeline
    g_pipeline = gst_pipeline_new("webcam-pipeline");

    // Create common elements
    source = gst_element_factory_make("v4l2src", "source");
    convert = gst_element_factory_make("videoconvert", "convert");
    tee = gst_element_factory_make("tee", "tee");
    display_queue = gst_element_factory_make("queue", "display_queue");
    display_sink = gst_element_factory_make("autovideosink", "display_sink");

    if (!g_pipeline || !source || !convert || !tee || !display_queue || !display_sink) {
        g_printerr("Failed to create basic pipeline elements.\n");
        return "Failed to create GStreamer elements";
    }

    // Set the device property on the source
    g_object_set(source,
        "device", device_path,
        "do-timestamp", TRUE,    // Use pipeline clock for timestamps
        NULL);

    // Configure tee to not block if one branch is slower
    g_object_set(tee,
        "allow-not-linked", TRUE,    // Don't fail if a branch returns not-linked
        NULL);

    // Configure display queue for minimal latency
    g_object_set(display_queue,
        "max-size-buffers", 2,      // Keep only 2 frames buffered for low latency
        "max-size-bytes", 0,         // Disable byte limit
        "max-size-time", 0,          // Disable time limit
        NULL);

    // Add basic elements to pipeline
    gst_bin_add_many(GST_BIN(g_pipeline), source, convert, tee, display_queue, display_sink, NULL);

    // Link: source -> convert -> tee
    if (!gst_element_link_many(source, convert, tee, NULL)) {
        g_printerr("Failed to link source -> convert -> tee.\n");
        gst_object_unref(g_pipeline);
        return "Failed to link GStreamer elements";
    }

    // Link display branch: tee -> display_queue -> display_sink
    tee_display_pad = gst_element_request_pad_simple(tee, "src_%u");
    queue_display_pad = gst_element_get_static_pad(display_queue, "sink");
    if (gst_pad_link(tee_display_pad, queue_display_pad) != GST_PAD_LINK_OK) {
        g_printerr("Failed to link tee to display queue.\n");
        gst_object_unref(g_pipeline);
        return "Failed to link display branch";
    }
    gst_object_unref(queue_display_pad);

    if (!gst_element_link(display_queue, display_sink)) {
        g_printerr("Failed to link display queue to sink.\n");
        gst_object_unref(g_pipeline);
        return "Failed to link display sink";
    }

    // If recording is enabled, add separate video and audio recording
    if (should_record) {
        g_print("Recording enabled - video: webcam_video.mp4, audio: webcam_audio.mp3\n");

        // Create video recording elements
        record_queue = gst_element_factory_make("queue", "record_queue");
        encoder = gst_element_factory_make("x264enc", "encoder");
        video_muxer = gst_element_factory_make("mp4mux", "video_muxer");
        video_file_sink = gst_element_factory_make("filesink", "video_file_sink");

        if (!record_queue || !encoder || !video_muxer || !video_file_sink) {
            g_printerr("Failed to create video recording elements.\n");
            gst_object_unref(g_pipeline);
            return "Failed to create video recording elements";
        }

        // Configure recording queue - leaky to prevent blocking
        g_object_set(record_queue,
            "max-size-buffers", 200,
            "max-size-bytes", 0,
            "max-size-time", 0,
            "leaky", 2,
            NULL);

        // Configure encoder
        g_object_set(encoder,
            "speed-preset", 6,          // Medium preset (0=ultrafast, 10=veryslow)
            "bitrate", 2048,            // 2 Mbps for good quality
            NULL);

        // Configure video filesink
        g_object_set(video_file_sink,
            "location", "webcam_video.mp4",
            "async", FALSE,
            NULL);

        // Add video recording elements to pipeline
        gst_bin_add_many(GST_BIN(g_pipeline), record_queue, encoder, video_muxer, video_file_sink, NULL);

        // Link video recording: tee -> record_queue -> encoder -> video_muxer -> video_file_sink
        tee_record_pad = gst_element_request_pad_simple(tee, "src_%u");
        queue_record_pad = gst_element_get_static_pad(record_queue, "sink");
        if (gst_pad_link(tee_record_pad, queue_record_pad) != GST_PAD_LINK_OK) {
            g_printerr("Failed to link tee to record queue.\n");
            gst_object_unref(g_pipeline);
            return "Failed to link record branch";
        }
        gst_object_unref(queue_record_pad);

        if (!gst_element_link_many(record_queue, encoder, video_muxer, video_file_sink, NULL)) {
            g_printerr("Failed to link video recording pipeline.\n");
            gst_object_unref(g_pipeline);
            return "Failed to link video recording pipeline";
        }

        // Create audio elements
        audio_src = gst_element_factory_make("pulsesrc", "audio_src");
        audio_convert = gst_element_factory_make("audioconvert", "audio_convert");
        audio_resample = gst_element_factory_make("audioresample", "audio_resample");
        audio_queue = gst_element_factory_make("queue", "audio_queue");
        audio_encoder = gst_element_factory_make("lamemp3enc", "audio_encoder");
        audio_file_sink = gst_element_factory_make("filesink", "audio_file_sink");

        if (!audio_src || !audio_convert || !audio_resample || !audio_queue || !audio_encoder || !audio_file_sink) {
            g_printerr("Failed to create audio elements. Audio will be disabled.\n");
            audio_src = NULL;
        } else {
            // Configure audio source
            g_object_set(audio_src,
                "do-timestamp", TRUE,
                "provide-clock", FALSE,
                "buffer-time", (gint64)200000,
                NULL);

            // Configure audio queue
            g_object_set(audio_queue,
                "max-size-buffers", 200,
                "leaky", 2,
                NULL);

            // Configure audio filesink
            g_object_set(audio_file_sink,
                "location", "webcam_audio.mp3",
                "async", FALSE,
                NULL);

            // Add audio elements to pipeline
            gst_bin_add_many(GST_BIN(g_pipeline), audio_src, audio_convert,
                           audio_resample, audio_queue, audio_encoder, audio_file_sink, NULL);

            // Link audio recording: audio_src -> audio_convert -> audio_resample -> audio_queue -> audio_encoder -> audio_file_sink
            if (!gst_element_link_many(audio_src, audio_convert, audio_resample,
                                      audio_queue, audio_encoder, audio_file_sink, NULL)) {
                g_printerr("Failed to link audio recording pipeline.\n");
                gst_object_unref(g_pipeline);
                return "Failed to link audio recording pipeline";
            }

            g_print("Audio recording enabled (default input device)\n");
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
