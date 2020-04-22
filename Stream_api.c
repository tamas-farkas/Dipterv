#include <gst/gst.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define CHUNK_SIZE 1080  /* Amount of bytes we are sending in each buffer */

/* Structure to contain all our information, so we can pass it to callbacks */
typedef struct _CustomData {
  GstElement *pipeline, *app_source;
  GstElement *rawvideoparse, *video_convert, *encoder, *video_sink;

  GMappedFile *file;
  guint8 *data;
  gsize length;
  guint64 offset;

  guint sourceid;        /* To control the GSource */

  GMainLoop *main_loop;  /* GLib's Main Loop */
} CustomData;

/* This method is called by the idle GSource in the mainloop, to feed CHUNK_SIZE bytes into appsrc.
 * The idle handler is added to the mainloop when appsrc requests us to start sending data (need-data signal)
 * and is removed when appsrc has enough data (enough-data signal).
 */
static gboolean push_data (CustomData *data) {
  GstBuffer *buffer;
  GstFlowReturn ret;
  guint len;

  if (data->offset >= data->length) {
    /* we are EOS, send end-of-stream and remove the source */
    g_signal_emit_by_name (data->app_source, "end-of-stream", &ret);
    g_main_loop_quit(data->main_loop);   //FIXME
    return FALSE;
  }

  /* read the next chunk */
  buffer = gst_buffer_new ();

  len = CHUNK_SIZE;
  if (data->offset + len > data->length)
    len = data->length - data->offset;

    gst_buffer_append_memory (buffer,
        gst_memory_new_wrapped (GST_MEMORY_FLAG_READONLY,
            data->data, data->length, data->offset, len, NULL, NULL));


  /* Push the buffer into the appsrc */
  g_signal_emit_by_name (data->app_source, "push-buffer", buffer, &ret);

  /* Free the buffer now that we are done with it */
  gst_buffer_unref (buffer);

  if (ret != GST_FLOW_OK) {
    /* We got some error, stop sending data */
    return FALSE;
  }

  data->offset += len;

  return TRUE;
}

/* This signal callback triggers when appsrc needs data. Here, we add an idle handler
 * to the mainloop to start pushing data into the appsrc */
static void start_feed (GstElement *source, guint size, CustomData *data) {
  if (data->sourceid == 0) {
    g_print ("Start feeding\n");
    data->sourceid = g_idle_add ((GSourceFunc) push_data, data);
  }
}

/* This callback triggers when appsrc has enough data and we can stop sending.
 * We remove the idle handler from the mainloop */
static void stop_feed (GstElement *source, CustomData *data) {
  if (data->sourceid != 0) {
    g_print ("Stop feeding\n");
    g_source_remove (data->sourceid);
    data->sourceid = 0;
  }
}


/* This function is called when an error message is posted on the bus */
static void error_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  GError *err;
  gchar *debug_info;

  /* Print error details on the screen */
  gst_message_parse_error (msg, &err, &debug_info);
  g_printerr ("Error received from element %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
  g_printerr ("Debugging information: %s\n", debug_info ? debug_info : "none");
  g_clear_error (&err);
  g_free (debug_info);

  g_main_loop_quit (data->main_loop);
}

int main(int argc, char *argv[]) {
  CustomData data;
  GstBus *bus;
  GError *error = NULL;

  /* Initialize cumstom data structure */
  memset (&data, 0, sizeof (data));

  /* Initialize GStreamer */
  gst_init (&argc, &argv);

  if (argc < 2) {
    g_print ("usage: %s <filename>\n", argv[0]);
    return -1;
  }

  CustomData *datap = &data;
  /* try to open the file as an mmapped file */
  datap->file = g_mapped_file_new (argv[1], FALSE, &error);
  if (error) {
    g_print ("failed to open file: %s\n", error->message);
    g_error_free (error);
    return -2;
  }
  /* get some vitals, this will be used to read data from the mmapped file and
   * feed it to appsrc. */
  data.length = g_mapped_file_get_length (data.file);
  data.data = (guint8 *) g_mapped_file_get_contents (data.file);
  data.offset = 0;

  /* Create the elements */
  data.app_source = gst_element_factory_make ("appsrc", "video_source");
  data.rawvideoparse = gst_element_factory_make ("rawvideoparse", "rawvideoparse");
  data.video_convert = gst_element_factory_make ("videoconvert", "video_convert");
  data.encoder = gst_element_factory_make ("x265enc", "encoder");
  data.video_sink = gst_element_factory_make ("filesink", "video_sink");

  /* Create the empty pipeline */
  data.pipeline = gst_pipeline_new ("test-pipeline");

  if (!data.pipeline || !data.app_source || !data.rawvideoparse || !data.video_convert || !data.encoder || !data.video_sink) {
    g_printerr ("Not all elements could be created.\n");
    return -1;
  }

  g_object_set (data.video_sink, "location", "/home/tamas/Documents/Dipterv/out.h265" , NULL);

  /* Configure appsrc */
  /*g_object_set (G_OBJECT (data.app_source), "caps",
   		gst_caps_new_simple ("video/x-raw",
 				     "format", G_TYPE_STRING, "I420",
 				     "width", G_TYPE_INT, 1920,
 				     "height", G_TYPE_INT, 1080,
 				     "framerate", GST_TYPE_FRACTION, 60, 1,
 				     NULL), NULL);
*/
  g_signal_connect (data.app_source, "need-data", G_CALLBACK (start_feed), &data);
  g_signal_connect (data.app_source, "enough-data", G_CALLBACK (stop_feed), &data);

  //g_object_set (data.rawvideoparse, "format", "I420", NULL);
  g_object_set (data.rawvideoparse, "width", 1920, NULL);
  g_object_set (data.rawvideoparse, "height", 1080, NULL);
  g_object_set (data.rawvideoparse, "framerate", 60, 1, NULL);

  /* Link all elements that can be automatically linked because they have "Always" pads */
  gst_bin_add_many (GST_BIN (data.pipeline), data.app_source, data.rawvideoparse, data.video_convert, data.encoder, data.video_sink, NULL);
  if (gst_element_link_many (data.app_source, data.rawvideoparse, data.video_convert, data.encoder, data.video_sink, NULL) != TRUE ) {
    g_printerr ("Elements could not be linked.\n");
    gst_object_unref (data.pipeline);
    return -1;
  }

  /* Instruct the bus to emit signals for each received message, and connect to the interesting signals */
  bus = gst_element_get_bus (data.pipeline);
  gst_bus_add_signal_watch (bus);
  g_signal_connect (G_OBJECT (bus), "message::error", (GCallback)error_cb, &data);
  gst_object_unref (bus);

  fprintf(stderr, "Setting main_loop_run to GST_STATE_PLAYING\n");
  /* Start playing the pipeline */
  gst_element_set_state (data.pipeline, GST_STATE_PLAYING);

  /* Create a GLib Main Loop and set it to run */
  data.main_loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (data.main_loop);

  fprintf(stderr, "main_loop_run returned, stopping playback\n");

  /* Free resources */
  gst_element_set_state (data.pipeline, GST_STATE_NULL);
  gst_object_unref (data.pipeline);
  return 0;
}
