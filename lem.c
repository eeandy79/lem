#include <gst/gst.h>
#include <glib.h>
#include <stdio.h>

static GQueue effects = G_QUEUE_INIT;
static GstPad *blockpad;
static GstElement *conv_before;
static GstElement *conv_after;
static GstElement *curr_effect;
static GstElement *pipeline;

static void
print_tag_foreach (const GstTagList * tags, const gchar * tag,
        gpointer user_data)
{
    GValue val = { 0, };
    gchar *str;
    gint depth = GPOINTER_TO_INT (user_data);

    if (!gst_tag_list_copy_value (&val, tags, tag))
        return;

    if (G_VALUE_HOLDS_STRING (&val))
        str = g_value_dup_string (&val);
    else
        str = gst_value_serialize (&val);

    g_print ("%*s%s: %s\n", 2 * depth, " ", gst_tag_get_nick (tag), str);
    g_free (str);

    g_value_unset (&val);
}

static void
dump_collection (GstStreamCollection * collection)
{
    guint i;
    GstTagList *tags;
    GstCaps *caps;

    for (i = 0; i < gst_stream_collection_get_size (collection); i++) {
        GstStream *stream = gst_stream_collection_get_stream (collection, i);
        g_print (" Stream %u type %s flags 0x%x\n", i,
                gst_stream_type_get_name (gst_stream_get_stream_type (stream)),
                gst_stream_get_stream_flags (stream));
        g_print ("  ID: %s\n", gst_stream_get_stream_id (stream));

        caps = gst_stream_get_caps (stream);
        if (caps) {
            gchar *caps_str = gst_caps_to_string (caps);
            g_print ("  caps: %s\n", caps_str);
            g_free (caps_str);
            gst_caps_unref (caps);
        }

        tags = gst_stream_get_tags (stream);
        if (tags) {
            g_print ("  tags:\n");
            gst_tag_list_foreach (tags, print_tag_foreach, GUINT_TO_POINTER (3));
            gst_tag_list_unref (tags);
        }
    }
}

static gboolean 
bus_cb (GstBus *bus, GstMessage *msg, gpointer data)
{
  GMainLoop *loop = (GMainLoop *) data;
  GstObject *src = GST_MESSAGE_SRC (msg);

  switch (GST_MESSAGE_TYPE (msg)) {

    case GST_MESSAGE_EOS:
      g_print ("End of stream\n");
      g_main_loop_quit (loop);
      break;

    case GST_MESSAGE_STREAM_COLLECTION:
    {
        GstStreamCollection *collection = NULL;
        gst_message_parse_stream_collection (msg, &collection);
        if (collection) {
            g_print ("Got a collection from %s:\n", src ? GST_OBJECT_NAME (src) : "Unknown");
            dump_collection (collection);
            /*
            if (data->collection && data->notify_id) {
                g_signal_handler_disconnect (data->collection, data->notify_id);
                data->notify_id = 0;
            }
            gst_object_replace ((GstObject **) & data->collection,
                    (GstObject *) collection);
            if (data->collection) {
                data->notify_id =
                    g_signal_connect (data->collection, "stream-notify",
                            (GCallback) stream_notify_cb, data);
            }
            if (data->timeout_id == 0)
                data->timeout_id =
                    g_timeout_add_seconds (5, (GSourceFunc) switch_streams, data);
            */
            gst_object_unref (collection);
        }
        //g_main_loop_quit (loop);
        break;
    }
    case GST_MESSAGE_ERROR: {
      gchar  *debug;
      GError *error;

      gst_message_parse_error (msg, &error, &debug);
      g_free (debug);

      g_printerr ("Error: %s\n", error->message);
      g_error_free (error);

      g_main_loop_quit (loop);
      break;
    }
    default:
      break;
  }

  return TRUE;
}

static GstPadProbeReturn
event_probe_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GMainLoop *loop = user_data;
  GstElement *next;

  if (GST_EVENT_TYPE (GST_PAD_PROBE_INFO_DATA (info)) != GST_EVENT_EOS)
    return GST_PAD_PROBE_PASS;

  gst_pad_remove_probe (pad, GST_PAD_PROBE_INFO_ID (info));

  g_queue_push_tail (&effects, gst_object_ref (curr_effect)); // push curr_effect -> tail
  next = g_queue_pop_head (&effects); // pop head for next_effect
  if (next == NULL) {
    GST_DEBUG_OBJECT (pad, "no more effects");
    g_main_loop_quit (loop);
    return GST_PAD_PROBE_DROP;
  }

  g_print ("Switching from '%s' to '%s'..\n", GST_OBJECT_NAME (curr_effect), GST_OBJECT_NAME (next));
  gst_element_set_state (curr_effect, GST_STATE_NULL);

  /* remove unlinks automatically */
  gst_bin_remove (GST_BIN (pipeline), curr_effect);
  gst_bin_add (GST_BIN (pipeline), next);
  gst_element_link_many (conv_before, next, conv_after, NULL);
  gst_element_set_state (next, GST_STATE_PLAYING);

  curr_effect = next;

  return GST_PAD_PROBE_DROP;
}


static GstPadProbeReturn
pad_probe_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstPad *srcpad, *sinkpad;

  gst_pad_remove_probe (pad, GST_PAD_PROBE_INFO_ID (info));

  /* install new probe for EOS */
  srcpad = gst_element_get_static_pad (curr_effect, "src");
  gst_pad_add_probe (srcpad, GST_PAD_PROBE_TYPE_BLOCK |
      GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, event_probe_cb, user_data, NULL);
  gst_object_unref (srcpad);

  /* push EOS and trigger event_probe_cb configured above */
  sinkpad = gst_element_get_static_pad (curr_effect, "sink");
  gst_pad_send_event (sinkpad, gst_event_new_eos ());
  gst_object_unref (sinkpad);

  return GST_PAD_PROBE_OK;
}


static gboolean
timeout_cb (gpointer user_data)
{
    gst_pad_add_probe (blockpad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
            pad_probe_cb, user_data, NULL);
    return TRUE;
}

static void
on_pad_added (GstElement *element,
              GstPad     *pad,
              gpointer    data)
{
/*
  GstPad *sinkpad;
  GstElement *decoder = (GstElement *) data;
  sinkpad = gst_element_get_static_pad (decoder, "sink");
  gst_pad_link (pad, sinkpad);
  gst_object_unref (sinkpad);
  */
  g_print("on_pad_add:\n");
}


void setupEffect()
{
#define DEFAULT_EFFECTS "exclusion,dodge,chromium,dilate,burn"
//#define DEFAULT_EFFECTS "burn,videoflip,agingtv"
  gchar **effect_names, **e;
  int i;
  GstElement* block;

  // setup the effect queue
  curr_effect = gst_bin_get_by_name( GST_BIN(pipeline), "effect");
  conv_before = gst_bin_get_by_name( GST_BIN(pipeline), "conv_before");
  printf("conv_before:%p\n", conv_before);
  conv_after = gst_bin_get_by_name( GST_BIN(pipeline), "conv_after");

  effect_names = g_strsplit (DEFAULT_EFFECTS, ",", -1);
  for (e = effect_names, i = 0; e != NULL && *e != NULL; ++e, i++) {
    GstElement *el;
    if (i == 0) {
        // this is the current one
    } else {
        el = gst_element_factory_make (*e, NULL);
    }
    if (el) {
      g_queue_push_tail (&effects, el);
    }
  }

  // setup the block pad for replacing effect element
  block = gst_bin_get_by_name( GST_BIN(pipeline), "q0");
  blockpad = gst_element_get_static_pad (block, "src");
}

int
main (int argc, char *argv[])
{
    GMainLoop *loop;
    GstBus *bus;
    guint bus_watch_id;

    /*
    char *c = "filesrc location=testclip.ts ! parsebin name=p ! decodebin name=d ! queue name=q0 ! autovideoconvert name=conv_before ! solarize name=effect ! autovideoconvert name=conv_after ! videoscale ! video/x-raw,width=640,height=480 ! x264enc bitrate=1000 ! mpegtsmux alignment=7 ! filesink location=dump.ts";
    */

    char *c = "filesrc location=testclip.ts ! parsebin name=p ! decodebin name=d ! queue name=q0 ! videoscale ! video/x-raw,width=640,height=480 ! x264enc bitrate=1000 name=enc ! mpegtsmux alignment=7 ! filesink location=dump.ts";
    /* Initialize GStreamer */
    gst_init (&argc, &argv);
    loop = g_main_loop_new (NULL, FALSE);

    /* Build the pipeline */
    pipeline = gst_parse_launch (c, NULL);

    /* Add a message handler */
    bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
    bus_watch_id = gst_bus_add_watch (bus, bus_cb, loop);
    gst_object_unref (bus);

    /* Effect initialization */
    setupEffect();
    {
        GstElement *a = gst_bin_get_by_name( GST_BIN(pipeline), "enc");
	printf("1:%p\n", a);
	GstElementClass *c = GST_ELEMENT_GET_CLASS(a);
	printf("2:%p\n", c);
	printf("\tAuthor: %s\n", gst_element_class_get_metadata(c, GST_ELEMENT_METADATA_AUTHOR));
	printf("\tDescription: %s\n", gst_element_class_get_metadata(c, GST_ELEMENT_METADATA_DESCRIPTION));
	printf("\turi: %s\n", gst_element_class_get_metadata(c, GST_ELEMENT_METADATA_DOC_URI));
	printf("\ticonname: %s\n", gst_element_class_get_metadata(c, GST_ELEMENT_METADATA_ICON_NAME));
	printf("\tklass: %s\n", gst_element_class_get_metadata(c, GST_ELEMENT_METADATA_KLASS));
	printf("\tlongname: %s\n", gst_element_class_get_metadata(c, GST_ELEMENT_METADATA_LONGNAME));

        //g_signal_connect (a, "pad-added", G_CALLBACK (on_pad_added), b);
    }
    
    /* Start playing */
    g_print ("Now playing ...\n");
    gst_element_set_state( pipeline, GST_STATE_PLAYING);
    GST_DEBUG_BIN_TO_DOT_FILE( GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "mygraph");

    /* Iterate */
    g_print ("Running...\n");
    //g_timeout_add_seconds (5, timeout_cb, loop);
    g_main_loop_run (loop);

    /* Out of the main loop, clean up nicely */
    g_print ("Returned, stopping playback\n");
    gst_element_set_state (pipeline, GST_STATE_NULL);
    gst_object_unref (pipeline);
    g_source_remove (bus_watch_id);
    g_main_loop_unref (loop);
    return 0;
}
