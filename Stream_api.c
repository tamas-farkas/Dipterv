// example appsrc for gstreamer 1.0 with own mainloop & external buffers. based on example from gstreamer docs.
// public domain, 2015 by Florian Echtler <floe@butterbrot.org>. compile with:
// gcc --std=c99 -Wall $(pkg-config --cflags gstreamer-1.0) -o gst gst.c $(pkg-config --libs gstreamer-1.0) -lgstapp-1.0

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include <stdint.h>


#define imgwidth 1920
#define imgheight 1080

/* Structure to contain all our information, so we can pass it to callbacks */
typedef struct _CustomData
{
  GstElement *pipeline;
  GstElement *source;
  GstElement *converter;
  GstElement *encoder;
  GstElement *packager;
  GstElement *sink;
} CustomData;


int want = 1;

uint16_t b_white[imgwidth*imgheight];
uint16_t b_black[imgwidth*imgheight];

static void prepare_buffer(GstAppSrc* appsrc) {

  static gboolean white = FALSE;
  static GstClockTime timestamp = 0;
  GstBuffer *buffer;
  guint size;
  GstFlowReturn ret;

  if (!want) return;
  want = 0;

  size = imgwidth * imgheight * 2;

  buffer = gst_buffer_new_wrapped_full( 0, (gpointer)(white?b_white:b_black), size, 0, size, NULL, NULL );

  white = !white;

  GST_BUFFER_PTS (buffer) = timestamp;
  GST_BUFFER_DURATION (buffer) = gst_util_uint64_scale_int (1, GST_SECOND, 4);

  timestamp += GST_BUFFER_DURATION (buffer);

  ret = gst_app_src_push_buffer(appsrc, buffer);

  if (ret != GST_FLOW_OK) {
    /* something wrong, stop pushing */
    // g_main_loop_quit (loop);
  }
}

static void cb_need_data (GstElement *appsrc, guint unused_size, gpointer user_data) {
  //prepare_buffer((GstAppSrc*)appsrc);
  want = 1;
}

gint main (gint argc, gchar *argv[]) {

  CustomData data;
  GstBus *bus;
  GstMessage *msg;
  GstStateChangeReturn ret;
  gboolean terminate = FALSE;

  for (int i = 0; i < imgwidth * imgheight; i++) { b_black[i] = 0; b_white[i] = 0xFFFF; }

  /* init GStreamer */
  gst_init (&argc, &argv);

  /* Create the elements */
  data.source = gst_element_factory_make ("appsrc", "source");
  data.converter = gst_element_factory_make ("videoconvert", "converter");
  data.encoder = gst_element_factory_make ("x265enc", "encoder");
  data.packager = gst_element_factory_make ("rtph265pay", "packager");
  data.sink = gst_element_factory_make ("udpsink", "sink");
//data.sink = gst_element_factory_make ("xvimagesink", "sink");

  /* Create the empty pipeline */
  data.pipeline = gst_pipeline_new ("test-pipeline");

  if (!data.pipeline || !data.source || !data.converter || !data.encoder || !data.packager || !data.sink) {
    g_printerr ("Not all elements could be created.\n");
    return -1;
  }

  /* setup */
  g_object_set (G_OBJECT (data.source), "caps",
  		gst_caps_new_simple ("video/x-raw",
				     "format", G_TYPE_STRING, "I420",
				     "width", G_TYPE_INT, imgwidth,
				     "height", G_TYPE_INT, imgheight,
				     "framerate", GST_TYPE_FRACTION, 60, 1,
				     NULL), NULL);
  /* Build the pipeline. Note that we are NOT linking the source at this
  * point. We will do it later. */
  gst_bin_add_many (GST_BIN (data.pipeline), data.source, data.converter, data.encoder, data.packager, data.sink, NULL);
  if (!gst_element_link_many (data.source, data.converter, data.encoder, data.packager, data.sink, NULL)) {
    g_printerr ("Elements could not be linked.\n");
    gst_object_unref (data.pipeline);
    return -1;
  }

  /* setup appsrc */
  g_object_set (G_OBJECT (data.source),
		"stream-type", 0, // GST_APP_STREAM_TYPE_STREAM
		"format", GST_FORMAT_TIME,
    "is-live", TRUE,
    NULL);
  g_signal_connect (data.source, "need-data", G_CALLBACK (cb_need_data), NULL);

  g_object_set (data.packager, "config-interval", 3, NULL);
  g_object_set (data.sink, "clients", "127.0.0.1:5200", NULL);
  g_object_set (data.sink, "sync", FALSE, NULL);

  /* Start playing */
  ret = gst_element_set_state (data.pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_printerr ("Unable to set the pipeline to the playing state.\n");
    gst_object_unref (data.pipeline);
    return -1;
  }

/* Listen to the bus */
  bus = gst_element_get_bus (data.pipeline);
do {
  msg = gst_bus_pop_filtered (bus,
      GST_MESSAGE_STATE_CHANGED | GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
  /* Parse message */
  if (msg != NULL) {
    GError *err;
    gchar *debug_info;

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_ERROR:
        gst_message_parse_error (msg, &err, &debug_info);
        g_printerr ("Error received from element %s: %s\n",
            GST_OBJECT_NAME (msg->src), err->message);
        g_printerr ("Debugging information: %s\n",
            debug_info ? debug_info : "none");
        g_clear_error (&err);
        g_free (debug_info);
        terminate = TRUE;
        break;
      case GST_MESSAGE_EOS:
        g_print ("End-Of-Stream reached.\n");
        terminate = TRUE;
        break;
      case GST_MESSAGE_STATE_CHANGED:
        /* We are only interested in state-changed messages from the pipeline */
        if (GST_MESSAGE_SRC (msg) == GST_OBJECT (data.pipeline)) {
          GstState old_state, new_state, pending_state;
          gst_message_parse_state_changed (msg, &old_state, &new_state,
              &pending_state);
          g_print ("Pipeline state changed from %s to %s:\n",
              gst_element_state_get_name (old_state),
              gst_element_state_get_name (new_state));
        }
        break;
      default:
        /* We should not reach here */
        g_printerr ("Unexpected message received.\n");
        break;
    }
    gst_message_unref (msg);
  }

  prepare_buffer((GstAppSrc*)data.source);
  g_main_context_iteration(g_main_context_default(),FALSE);

} while (!terminate);

  /* Free resources */
  gst_object_unref (bus);
  gst_element_set_state (data.pipeline, GST_STATE_NULL);
  gst_object_unref (data.pipeline);
  return 0;
}
