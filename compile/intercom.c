#include <stdio.h>
#include <signal.h>
#include <gst/gst.h>
#include <sys/stat.h>
#include <ncurses.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>

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

static WINDOW *log_window;

static void setup_log_window()
{
    log_window = newwin(LINES, COLS, 0, 0); // Create a new window with full terminal dimensions
    if (!log_window)
    {
        fprintf(stderr, "Failed to create log window\n");
        exit(1);
    }

    scrollok(log_window, TRUE); // Enable scrolling
    wrefresh(log_window);       // Refresh the window to make it visible
}

static int log_file_fd = -1; // File descriptor for the log file

static void setup_log_file(const char *file_path)
{
    log_file_fd = open(file_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_file_fd == -1)
    {
        fprintf(stderr, "Failed to open log file: %s\n", file_path);
        exit(1);
    }

    char time_buffer[64];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(time_buffer, sizeof(time_buffer) - 1, "%Y-%m-%d %H:%M:%S", t);
    dprintf(log_file_fd, "\nStarting new log session:\n");
    dprintf(log_file_fd, "[TIMESTAMP] %s\n\n", time_buffer);
}

static void close_log_file()
{
    if (log_file_fd != -1)
    {
        close(log_file_fd);
        log_file_fd = -1;
    }
}

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for thread safety

static void write_to_log_window(const char *message)
{
    pthread_mutex_lock(&log_mutex); // Lock the mutex for thread safety

    // Log to ncurses window
    if (log_window)
    {
        wprintw(log_window, message);
        wrefresh(log_window);
    } 
    else
    {
        printf("%s", message);
    }

    // Log to file
    if (log_file_fd != -1)
    {
        // Write to log file
        write(log_file_fd, message, strlen(message));
    }
    
    pthread_mutex_unlock(&log_mutex); // Unlock the mutex
}

static void print_log(const char *format, ...)
{
    va_list args;
    va_start(args, format); // Initialize the va_list
    char *full_msg;
    vasprintf(&full_msg, format, args); // Use the va_list
    va_end(args); // Clean up the va_list

    // Prepend prefix
    char *final_msg;
    asprintf(&final_msg, "%s%s", "[LOG] ", full_msg);

    write_to_log_window(final_msg);
    free(full_msg);
    free(final_msg);
}

static void print_warning(const char *format, ...)
{
    va_list args;
    va_start(args, format); // Initialize the va_list
    char *full_msg;
    vasprintf(&full_msg, format, args); // Use the va_list
    va_end(args); // Clean up the va_list

    // Prepend prefix
    char *final_msg;
    asprintf(&final_msg, "%s%s", "[WARNING] ", full_msg);

    write_to_log_window(final_msg);
    free(full_msg);
    free(final_msg);
}

static void print_error(const char *format, ...)
{
    va_list args;
    va_start(args, format); // Initialize the va_list
    char *full_msg;
    vasprintf(&full_msg, format, args); // Use the va_list
    va_end(args); // Clean up the va_list

    // Prepend prefix
    char *final_msg;
    asprintf(&final_msg, "%s%s", "[ERROR] ", full_msg);

    write_to_log_window(final_msg);
    free(full_msg);
    free(final_msg);
}

static void print_props(GObject *object)
{
    if (!object)
    {
        print_warning("Cannot print properties of a NULL object.\n");
        return;
    }

    GParamSpec **props;
    guint n_props;

    props = g_object_class_list_properties(G_OBJECT_GET_CLASS(object), &n_props);
    if (!props || n_props == 0)
    {
        print_warning("No properties found for the given object.\n");
        return;
    }

    print_log("Object properties:\n");
    for (guint i = 0; i < n_props; i++)
    {
        print_log(" - %s\n", props[i]->name);
    }

    g_free(props);
}

static void print_element_properties(GstElement *element)
{
    if (!element)
    {
        print_warning("Cannot print properties of a NULL GstElement.\n");
        return;
    }

    // Print properties of the GstElement itself
    print_log("Properties of element: %s\n", GST_ELEMENT_NAME(element));
    print_props(G_OBJECT(element));

    // Iterate over pads and print their properties
    GstIterator *pad_iter = gst_element_iterate_pads(element);
    GValue pad_item = G_VALUE_INIT;
    gboolean done = FALSE;

    while (!done)
    {
        switch (gst_iterator_next(pad_iter, &pad_item))
        {
        case GST_ITERATOR_OK:
        {
            GstPad *pad = GST_PAD(g_value_get_object(&pad_item));
            print_log("Properties of pad: %s\n", GST_PAD_NAME(pad));
            print_props(G_OBJECT(pad));
            g_value_reset(&pad_item);
            break;
        }
        case GST_ITERATOR_RESYNC:
            gst_iterator_resync(pad_iter);
            break;
        case GST_ITERATOR_ERROR:
            print_error("Error iterating over pads.\n");
            done = TRUE;
            break;
        case GST_ITERATOR_DONE:
            done = TRUE;
            break;
        }
    }

    g_value_unset(&pad_item);
    gst_iterator_free(pad_iter);

    // If the element is a GstBin, iterate over its children
    if (GST_IS_BIN(element))
    {
        GstIterator *child_iter = gst_bin_iterate_elements(GST_BIN(element));
        GValue child_item = G_VALUE_INIT;
        done = FALSE;

        while (!done)
        {
            switch (gst_iterator_next(child_iter, &child_item))
            {
            case GST_ITERATOR_OK:
            {
                GstElement *child = GST_ELEMENT(g_value_get_object(&child_item));
                print_log("\nChild element: %s\n", GST_ELEMENT_NAME(child));
                print_element_properties(child); // Recursive call
                g_value_reset(&child_item);
                break;
            }
            case GST_ITERATOR_RESYNC:
                gst_iterator_resync(child_iter);
                break;
            case GST_ITERATOR_ERROR:
                print_error("Error iterating over child elements.\n");
                done = TRUE;
                break;
            case GST_ITERATOR_DONE:
                done = TRUE;
                break;
            }
        }

        g_value_unset(&child_item);
        gst_iterator_free(child_iter);
    }
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
                // Do nothing for other keys
                break;
            }
        }
        g_usleep(100000); // Sleep for 100ms to reduce CPU usage
    }
    return NULL;
}

static void handle_resize(int sig)
{
    refresh(); // Refresh ncurses to apply new dimensions
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

    setup_log_window();

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
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "Received signal %d (%s), cleaning up...\n", signum, signal_name ? signal_name : "Unknown");
    print_log(buffer);

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
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "Error: %s\n", err->message);
            print_error(buffer);
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
char *start_cam(char *device_path, int should_record)
{
    // print setup values
    fprintf(stdout, "Starting webcam with device: %s\n", device_path);
    fprintf(stdout, "Recording enabled: %s\n", should_record ? "Yes" : "No");

    char *error;

    setup_log_file("intercom_log.txt");

    {
        // Setup window key events
        error = setup_ncurses();
        if (error != NULL)
        {
            print_warning("Warning: Failed to setup ncurses: %s\n", error);
            return error;
        }
    }

    GstElement *source, *decoder, *convert, *tee, *display_queue, *display_sink;
    GstBus *bus;

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

    decoder = gst_element_factory_make("jpegdec", "decoder");
    if (!decoder)
    {
        print_error("Failed to create jpegdec. Is gst-plugins-good installed?\n");
        return "Missing jpegdec GStreamer plugin";
    }

    if (!g_pipeline || !source || !convert || !tee || !display_queue || !display_sink)
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

    // Configure tee to not block if one branch is slower
    g_object_set(tee,
                 "allow-not-linked", TRUE, // Don't fail if a branch returns not-linked
                 NULL);

    // Add basic elements to pipeline
    gst_bin_add_many(GST_BIN(g_pipeline), source, decoder, convert, tee, display_queue, display_sink, NULL);

    // Link: source -> decoder -> convert -> tee
    if (!gst_element_link_many(source, decoder, convert, tee, NULL))
    {
        print_error("Failed to link source -> decoder -> convert -> tee.\n");
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

    endwin(); // End ncurses

    close_log_file();

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