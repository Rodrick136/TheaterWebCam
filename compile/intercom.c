#include <stdio.h>
#include <signal.h>
#include <gst/gst.h>
#include <sys/stat.h>
#include <ncurses.h>
#include <pthread.h>

static GMainLoop *g_main_loop = NULL;
static GstElement *g_pipeline = NULL;
static GstElement *g_voice_pipeline = NULL; // Separate pipeline for voice audio
static GstElement *g_fx_pipeline = NULL;    // Separate pipeline for effects audio

// Global variable to track the last sync marker timestamp
static GstClockTime g_last_sync_marker = GST_CLOCK_TIME_NONE;
static GMutex g_sync_marker_mutex; // Mutex for thread-safe access to sync marker

// Forward declarations
static void configure_audio_pipeline(const char *name, const char *client_name, const char *output_file, GstElement **pipeline_ptr);
static char *setup_display_branch(GstElement *tee, GstElement *display_queue, GstElement *display_sink);
static char *setup_recording_branch(GstElement *tee);

static WINDOW *log_pad;
static int log_pad_height = 1000; // Initial height of the pad
static int log_pad_width;
static int log_pad_current_line = 0; // Tracks the current line for writing logs

static void setup_log_pad()
{
    log_pad_width = COLS; // Use the full width of the terminal
    log_pad_height = LINES; // Use the full height of the terminal
    log_pad = newpad(log_pad_height, log_pad_width);
    if (!log_pad)
    {
        fprintf(stderr, "Failed to create log pad\n");
        exit(1);
    }
    scrollok(log_pad, TRUE); // Enable scrolling

    // Ensure the log_pad is always visible by refreshing the entire screen
    prefresh(log_pad, 0, 0, 0, 0, LINES - 1, COLS - 1);
}

static void write_to_log_pad(const char *prefix, const char *format, va_list args)
{
    if (log_pad)
    {
        char message[1024];
        vsnprintf(message, sizeof(message), format, args);

        // Write to the pad with prefix
        mvwprintw(log_pad, log_pad_current_line++, 0, "%s%s", prefix, message);
        if (log_pad_current_line >= log_pad_height)
        {
            log_pad_height *= 2; // Double the pad height if we exceed it
            wresize(log_pad, log_pad_height, log_pad_width);
        }

        // Refresh the visible portion of the pad
        prefresh(log_pad, log_pad_current_line - LINES + 5, 0, 0, 0, LINES - 1, COLS - 1);
    }
    else
    {
        // Fallback to standard output if log_pad is not initialized
        fprintf(stdout, "%s", prefix);
        vfprintf(stdout, format, args);
        fprintf(stdout, "\n");
    }
}

static void print_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    write_to_log_pad("[LOG] ", format, args);
    va_end(args);
}

static void print_warning(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    write_to_log_pad("[WARNING] ", format, args);
    va_end(args);
}

static void print_error(const char *format, ...)
{

    va_list args;
    va_start(args, format);
    write_to_log_pad("[ERROR] ", format, args);
    va_end(args);
}

static void *ncurses_event_listener(void *arg)
{
    int ch;
    while (1)
    {
        ch = getch();
        if (ch != ERR)
        {
            switch (ch)
            {
            case 'm':
                print_log("[KEY] m (make markers)\n");
                break;
            case 'q':
                print_log("[KEY] q (quit)\n");
                raise(SIGTERM); // Send SIGTERM to exit the main loop
                pthread_exit(NULL);
            case 3: // ASCII value for Ctrl+C
                print_log("[KEY] Ctrl+C (quit)\n");
                raise(SIGTERM); // Send SIGTERM to exit the main loop
                pthread_exit(NULL);
            default:
                // print_log("Key pressed: %c\n", ch);
                //  Do nothing for other keys
                break;
            }
        }
        g_usleep(100000); // Sleep for 100ms to reduce CPU usage
    }
    return NULL;
}

static void handle_resize(int sig)
{
    endwin(); // End ncurses mode to reset terminal dimensions
    refresh(); // Refresh ncurses to apply new dimensions

    log_pad_width = COLS; // Update log_pad dimensions to match new terminal size
    log_pad_height = LINES;
    wresize(log_pad, log_pad_height, log_pad_width); // Resize the pad

    // Ensure the log_pad is always visible by refreshing the entire screen
    prefresh(log_pad, 0, 0, 0, 0, LINES - 1, COLS - 1);
}

static char *setup_ncurses()
{
    // Disable default SIGINT handling to allow Ctrl+C to be captured as a key press
    // signal(SIGINT, SIG_IGN);

    // Initialize ncurses
    initscr();
    raw(); // Use raw mode to capture all key presses, including Ctrl+C
    noecho();
    curs_set(1);           // Show the cursor
    nodelay(stdscr, TRUE); // Non-blocking input
    keypad(stdscr, TRUE);

    setup_log_pad();

    // Set up a signal handler for window resize events
    signal(SIGWINCH, handle_resize);

    // Start a thread to listen for terminal events
    pthread_t ncurses_thread;
    if (pthread_create(&ncurses_thread, NULL, ncurses_event_listener, NULL) != 0)
    {
        print_error("Error: Failed to create ncurses event listener thread.\n");
    }

    return NULL;
}

static gboolean is_display_window_closed(const GError *err)
{
    return err && err->message && g_strrstr(err->message, "window was closed") != NULL;
}

static void signal_handler(int signum)
{
    const char *signal_name = strsignal(signum);
    print_log("Received signal %d (%s), cleaning up...\n", signum, signal_name ? signal_name : "Unknown");

    if (g_main_loop)
    {
        g_main_loop_quit(g_main_loop);
    }
}

static gboolean bus_callback(GstBus *bus, GstMessage *message, gpointer data)
{
    switch (GST_MESSAGE_TYPE(message))
    {
    case GST_MESSAGE_ERROR:
    {
        GError *err;
        gchar *debug;
        gst_message_parse_error(message, &err, &debug);
        if (is_display_window_closed(err))
        {
            print_log("Display window closed, stopping...\n");
        }
        else
        {
            print_error("Error: %s\n", err->message);
        }
        g_error_free(err);
        g_free(debug);
        raise(SIGTERM); // Send SIGTERM to exit the main loop
        break;
    }
    case GST_MESSAGE_EOS:
        print_log("End of stream\n");
        if (g_main_loop)
        {
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
    char *error;

    {
        // Setup window key events
        error = setup_ncurses();
        if (error != NULL)
        {
            print_warning("Warning: Failed to setup ncurses: %s\n", error);
            return error;
        }
    }

    GstElement *source, *capsfilter, *decoder, *convert, *tee, *display_queue, *display_sink;
    GstBus *bus;

    // Initialize GStreamer
    gst_init(NULL, NULL);

    // Create the empty pipeline
    g_pipeline = gst_pipeline_new("webcam-pipeline");

    // Create common elements
    source = gst_element_factory_make("v4l2src", "source");
    capsfilter = gst_element_factory_make("capsfilter", "capsfilter");
    convert = gst_element_factory_make("videoconvert", "convert");
    tee = gst_element_factory_make("tee", "tee");
    display_queue = gst_element_factory_make("queue", "display_queue");
    display_sink = gst_element_factory_make("autovideosink", "display_sink");

    decoder = gst_element_factory_make("jpegdec", "decoder");
    if (!decoder)
    {
        print_error("Failed to create jpegdec. Is gst-plugins-good installed?\n");
        return "Missing jpegdec GStreamer plugin";
    }

    if (!g_pipeline || !source || !capsfilter || !convert || !tee || !display_queue || !display_sink)
    {
        print_error("Failed to create basic pipeline elements.\n");
        return "Failed to create GStreamer elements";
    }

    // Set the device property on the source
    g_object_set(source,
                 "device", device_path,
                 "do-timestamp", TRUE, // Use pipeline clock for timestamps
                 "io-mode", 2,         // Prefer MMAP for lower overhead if supported
                 NULL);

    // Enable QoS so transforms can drop late frames
    g_object_set(convert,
                 "qos", TRUE,
                 NULL);

    // Parse and set video size via capsfilter (not on v4l2src)
    int width, height;
    if (sscanf(video_size, "%dx%d", &width, &height) == 2)
    {
        GstCaps *caps = gst_caps_new_simple("image/jpeg",
                                            "width", G_TYPE_INT, width,
                                            "height", G_TYPE_INT, height,
                                            "framerate", GST_TYPE_FRACTION, 30, 1,
                                            NULL);
        g_object_set(capsfilter, "caps", caps, NULL);
        gst_caps_unref(caps);
        print_log("Video size set to: %dx%d\n", width, height);
    }
    else
    {
        print_error("Invalid video size format: %s\n", video_size);
        return "Invalid video size format. Expected \\d+x\\d+.";
    }

    // Configure tee to not block if one branch is slower
    g_object_set(tee,
                 "allow-not-linked", TRUE, // Don't fail if a branch returns not-linked
                 NULL);

    // Add basic elements to pipeline
    gst_bin_add_many(GST_BIN(g_pipeline), source, capsfilter, decoder, convert, tee, display_queue, display_sink, NULL);

    // Link: source -> capsfilter -> decoder -> convert -> tee
    if (!gst_element_link_many(source, capsfilter, decoder, convert, tee, NULL))
    {
        print_error("Failed to link source -> capsfilter -> decoder -> convert -> tee.\n");
        gst_object_unref(g_pipeline);
        return "Failed to link GStreamer elements";
    }

    // Setup display branch
    error = setup_display_branch(tee, display_queue, display_sink);
    if (error != NULL)
    {
        gst_object_unref(g_pipeline);
        return error;
    }

    // If recording is enabled, setup recording branch
    if (should_record)
    {
        error = setup_recording_branch(tee);
        if (error != NULL)
        {
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
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        print_error("Unable to set the pipeline to the playing state.\n");
        gst_object_unref(g_pipeline);
        return "Failed to start pipeline";
    }

    // Setup signal handlers for cleanup
    signal(SIGTERM, signal_handler);

    // Initialize mutex for sync marker
    g_mutex_init(&g_sync_marker_mutex);

    // Create and run the main loop
    g_main_loop = g_main_loop_new(NULL, FALSE);
    print_log("Running webcam stream. Press q or Ctrl+C to stop.\n");
    g_main_loop_run(g_main_loop);

    // Start Cleanup
    print_log("Cleaning up...\n");

    // Destroy the mutex during cleanup
    g_mutex_clear(&g_sync_marker_mutex);

    // Send end-of-stream event to video pipeline to finalize files
    gst_element_send_event(g_pipeline, gst_event_new_eos());

    // Wait a bit for EOS to be processed
    GstBus *cleanup_bus = gst_element_get_bus(g_pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(cleanup_bus,
                                                 2 * GST_SECOND,
                                                 GST_MESSAGE_EOS | GST_MESSAGE_ERROR);

    if (msg)
    {
        gst_message_unref(msg);
    }
    gst_object_unref(cleanup_bus);

    gst_element_set_state(g_pipeline, GST_STATE_NULL);
    gst_object_unref(g_pipeline);

    // Cleanup audio pipelines if they exist
    if (g_voice_pipeline)
    {
        gst_element_send_event(g_voice_pipeline, gst_event_new_eos());
        GstBus *voice_bus = gst_element_get_bus(g_voice_pipeline);
        GstMessage *voice_msg = gst_bus_timed_pop_filtered(voice_bus,
                                                           2 * GST_SECOND,
                                                           GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
        if (voice_msg)
        {
            gst_message_unref(voice_msg);
        }
        gst_object_unref(voice_bus);
        gst_element_set_state(g_voice_pipeline, GST_STATE_NULL);
        gst_object_unref(g_voice_pipeline);
    }

    if (g_fx_pipeline)
    {
        gst_element_send_event(g_fx_pipeline, gst_event_new_eos());
        GstBus *fx_bus = gst_element_get_bus(g_fx_pipeline);
        GstMessage *fx_msg = gst_bus_timed_pop_filtered(fx_bus,
                                                        2 * GST_SECOND,
                                                        GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
        if (fx_msg)
        {
            gst_message_unref(fx_msg);
        }
        gst_object_unref(fx_bus);
        gst_element_set_state(g_fx_pipeline, GST_STATE_NULL);
        gst_object_unref(g_fx_pipeline);
    }

    g_main_loop_unref(g_main_loop);

    // Store all logs from the pad into a buffer before ending ncurses
    if (log_pad)
    {
        int height, width;
        getmaxyx(log_pad, height, width);

        char **buffer = malloc(log_pad_current_line * sizeof(char *));
        for (int i = 0; i < log_pad_current_line; i++)
        {
            buffer[i] = malloc((width + 1) * sizeof(char)); // +1 for null terminator
            memset(buffer[i], 0, width + 1);
            mvwinnstr(log_pad, i, 0, buffer[i], width);
        }

        delwin(log_pad);
        log_pad = NULL;
        endwin();

        // Print the buffered lines to stdout
        for (int i = 0; i < log_pad_current_line; i++)
        {
            printf("%s\n", buffer[i]);
            free(buffer[i]); // Free each line after printing
        }
        free(buffer); // Free the buffer array
    }
    else
    {
        endwin();
    }

    return NULL;
}

static char *setup_display_branch(GstElement *tee, GstElement *display_queue, GstElement *display_sink)
{
    GstPad *tee_display_pad, *queue_display_pad;

    // Configure display queue for minimal latency
    g_object_set(display_queue,
                 "max-size-buffers", 2, // Keep only 2 frames buffered for low latency
                 "max-size-bytes", 0,   // Disable byte limit
                 "max-size-time", 0,    // Disable time limit
                 "leaky", 2,            // Drop oldest buffers when downstream is late
                 NULL);

    // Display ASAP without clock sync to minimize latency
    g_object_set(display_sink,
                 "sync", FALSE,
                 NULL);

    // Link display branch: tee -> display_queue -> display_sink
    tee_display_pad = gst_element_request_pad_simple(tee, "src_%u");
    queue_display_pad = gst_element_get_static_pad(display_queue, "sink");
    if (gst_pad_link(tee_display_pad, queue_display_pad) != GST_PAD_LINK_OK)
    {
        print_error("Failed to link tee to display queue.\n");
        gst_object_unref(queue_display_pad);
        return "Failed to link display branch";
    }
    gst_object_unref(queue_display_pad);

    if (!gst_element_link(display_queue, display_sink))
    {
        print_error("Failed to link display queue to sink.\n");
        return "Failed to link display sink";
    }

    return NULL;
}

static char *setup_recording_branch(GstElement *tee)
{
    GstElement *record_queue, *encoder, *muxer_queue, *video_muxer, *video_file_sink;
    GstPad *tee_record_pad, *queue_record_pad;

    print_log("Recording enabled - video: webcam_video.mp4\n");

    // Create video recording elements
    record_queue = gst_element_factory_make("queue", "record_queue");
    encoder = gst_element_factory_make("x264enc", "encoder");
    muxer_queue = gst_element_factory_make("queue", "muxer_queue"); // Additional queue for decoupling
    video_muxer = gst_element_factory_make("mp4mux", "video_muxer");
    video_file_sink = gst_element_factory_make("filesink", "video_file_sink");

    if (!record_queue || !encoder || !muxer_queue || !video_muxer || !video_file_sink)
    {
        print_error("Failed to create video recording elements.\n");
        return "Failed to create video recording elements";
    }

    // Configure recording queue - leaky to prevent blocking
    g_object_set(record_queue,
                 "max-size-buffers", 200,
                 "max-size-bytes", 0,
                 "max-size-time", 0,
                 "leaky", 2,
                 NULL);

    // Configure muxer queue - also leaky to decouple muxer from encoder
    g_object_set(muxer_queue,
                 "max-size-buffers", 100,
                 "max-size-bytes", 0,
                 "max-size-time", 0,
                 "leaky", 2,
                 NULL);

    // Configure encoder for AVCC + low-latency
    g_object_set(encoder,
                 //"byte-stream", FALSE, // AVCC format required by mp4mux
                 "speed-preset", 6, // ultrafast
                 "bitrate", 2048,   // ~2 Mbps
                 //"key-int-max", 30,    // frequent IDR
                 //"bframes", 0,         // no reordering
                 //"rc-lookahead", 0,    // no lookahead
                 NULL);
    // Set tune=zerolatency if available (parse via util to avoid enum mismatch)
    {
        GParamSpec *ps = g_object_class_find_property(G_OBJECT_GET_CLASS(encoder), "tune");
        if (ps)
        {
            gst_util_set_object_arg(G_OBJECT(encoder), "tune", "zerolatency");
        }
    }

    // Configure video filesink
    // create new folder for each new recording session

    char *name = "webcam_video.mp4";
    char foldername[50];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(foldername, sizeof(foldername) - 1, "Recording__%Y-%m-%dT%H:%M:%S", t);
    mkdir(foldername, 0777); // create folder
    char filepath[100];
    snprintf(filepath, sizeof(filepath), "%s/%s", foldername, name);
    g_object_set(video_file_sink,
                 "location", filepath,
                 "async", FALSE,
                 NULL);

    // Optional: mp4mux faststart if supported
    {
        GParamSpec *ps = g_object_class_find_property(G_OBJECT_GET_CLASS(video_muxer), "faststart");
        if (ps)
        {
            g_object_set(video_muxer, "faststart", TRUE, NULL);
        }
    }

    // Add video recording elements to pipeline
    gst_bin_add_many(GST_BIN(g_pipeline), record_queue, encoder, muxer_queue, video_muxer, video_file_sink, NULL);

    // Link video recording: tee -> record_queue -> encoder -> muxer_queue -> video_muxer -> video_file_sink
    tee_record_pad = gst_element_request_pad_simple(tee, "src_%u");
    queue_record_pad = gst_element_get_static_pad(record_queue, "sink");
    if (gst_pad_link(tee_record_pad, queue_record_pad) != GST_PAD_LINK_OK)
    {
        print_error("Failed to link tee to record queue.\n");
        gst_object_unref(queue_record_pad);
        return "Failed to link record branch";
    }
    gst_object_unref(queue_record_pad);

    if (!gst_element_link_many(record_queue, encoder, muxer_queue, video_muxer, video_file_sink, NULL))
    {
        print_error("Failed to link video recording pipeline.\n");
        return "Failed to link video recording pipeline";
    }

    // Configure and attach audio recording branch (non-fatal on failure)
    char voice_out[100];
    snprintf(voice_out, sizeof(voice_out), "%s/%s", foldername, "webcam_voice.mp3");
    configure_audio_pipeline("Voice", "Voice In", voice_out, &g_voice_pipeline);

    char effects_out[100];
    snprintf(effects_out, sizeof(effects_out), "%s/%s", foldername, "webcam_effects.mp3");
    configure_audio_pipeline("Effects", "Effects In", effects_out, &g_fx_pipeline);

    return NULL;
}

static void configure_audio_pipeline(
    const char *name,
    const char *client_name,
    const char *output_file,
    GstElement **pipeline_ptr)
{
    GstElement *src, *src_queue, *convert, *resample, *encoder, *file_sink;
    char pipeline_name[64];

    snprintf(pipeline_name, sizeof(pipeline_name), "%s-audio-pipeline", name);

    // Pipeline: autoaudiosrc -> queue -> convert -> resample -> encoder -> file
    src = gst_element_factory_make("autoaudiosrc", "source");
    src_queue = gst_element_factory_make("queue", NULL);
    convert = gst_element_factory_make("audioconvert", NULL);
    resample = gst_element_factory_make("audioresample", NULL);
    encoder = gst_element_factory_make("lamemp3enc", NULL);
    file_sink = gst_element_factory_make("filesink", NULL);

    if (!src || !src_queue || !convert || !resample || !encoder || !file_sink)
    {
        print_error("Failed to create %s audio elements.\n", name);
        return;
    }

    // Configure source queue to buffer audio and prevent blocking
    g_object_set(src_queue,
                 "max-size-buffers", 200,
                 "max-size-bytes", 0,
                 "max-size-time", 0,
                 "leaky", 2, // Leak old buffers if queue is full
                 NULL);

    // Create the pipeline
    *pipeline_ptr = gst_pipeline_new(pipeline_name);
    // Configure encoder
    g_object_set(encoder,
                 "target", 1,
                 "bitrate", 128,
                 "cbr", TRUE,
                 NULL);

    // Configure file sink
    g_object_set(file_sink,
                 "location", output_file,
                 "sync", FALSE,
                 NULL);

    // Add elements to pipeline
    gst_bin_add_many(GST_BIN(*pipeline_ptr),
                     src, src_queue, convert, resample,
                     encoder, file_sink,
                     NULL);

    // Link: source -> queue -> convert -> resample -> encoder -> file
    if (!gst_element_link_many(src, src_queue, convert, resample,
                               encoder, file_sink, NULL))
    {
        print_error("Failed to link %s audio pipeline.\n", name);
        gst_object_unref(*pipeline_ptr);
        *pipeline_ptr = NULL;
        return;
    }

    print_log("[%s Audio] Pipeline created successfully\n", name);
    // Try to configure autoaudiosrc with device and client name properties
    // These need to be set BEFORE the pipeline goes to READY
    // g_object_set(src, "client-name", client_name, NULL);
    // print_log("[%s Audio] Set client-name property on autoaudiosrc\n", name);
    gst_element_set_state(*pipeline_ptr, GST_STATE_READY);

    GObject *child = gst_child_proxy_get_child_by_index(GST_CHILD_PROXY(src), 0);
    if (child)
    {
        GObjectClass *child_class = G_OBJECT_GET_CLASS(child);

        // Check for 'stream-properties' directly
        if (g_object_class_find_property(child_class, "stream-properties"))
        {
            // Create PipeWire/PulseAudio metadata structure
            GstStructure *s = gst_structure_new("props",
                                                "node.name", G_TYPE_STRING, client_name,
                                                "node.description", G_TYPE_STRING, client_name,
                                                "media.name", G_TYPE_STRING, client_name,
                                                NULL);

            g_object_set(child, "stream-properties", s, NULL);
            gst_structure_free(s);
            print_log("Successfully set stream-properties for qpwgraph\n");
        }

        // Also check for 'client-name' for the server connection
        if (g_object_class_find_property(child_class, "client-name"))
        {
            g_object_set(child, "client-name", client_name, NULL);
        }

        g_object_unref(child);
    }

    // Now start the pipeline
    GstStateChangeReturn ret = gst_element_set_state(*pipeline_ptr, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        print_error("Failed to start %s audio pipeline.\n", name);
        gst_object_unref(*pipeline_ptr);
        *pipeline_ptr = NULL;
    }
    else
    {
        print_log("[%s Audio] Started - recording to %s\n", name, output_file);
    }
}
