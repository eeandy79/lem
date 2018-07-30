#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gst/gst.h>
#include <stdlib.h>
#include <stdio.h>


#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

#include "gstmyfilter.h"
//#include "tfidecklinksrc.h"
//#include "decklinkconfig.h"

//static IDeckLinkInput*   g_deckLinkInput = NULL;
//static BMDConfig        g_config;


GST_DEBUG_CATEGORY_STATIC (gst_my_filter_debug);
#define GST_CAT_DEFAULT gst_my_filter_debug

/* Filter signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  PROP_0,
  PROP_SILENT
};

/* the capabilities of the inputs and outputs.
 *
 * describe the real formats here.
 */
static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("ANY")
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("ANY")
    );

#define gst_my_filter_parent_class parent_class
G_DEFINE_TYPE (GstMyFilter, gst_my_filter, GST_TYPE_ELEMENT);

static void gst_my_filter_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_my_filter_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_my_filter_sink_event (GstPad * pad, GstObject * parent, GstEvent * event);
static GstFlowReturn gst_my_filter_chain (GstPad * pad, GstObject * parent, GstBuffer * buf);

static gboolean gst_base_src_query (GstPad * pad, GstObject * parent, GstQuery * query);
/* GObject vmethod implementations */

/* initialize the myfilter's class */
static void
gst_my_filter_class_init (GstMyFilterClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = gst_my_filter_set_property;
  gobject_class->get_property = gst_my_filter_get_property;

  g_object_class_install_property (gobject_class, PROP_SILENT,
      g_param_spec_boolean ("silent", "Silent", "Produce verbose output ?",
          FALSE, G_PARAM_READWRITE));

  gst_element_class_set_details_simple(gstelement_class,
    "MyFilter",
    "FIXME:Generic",
    "FIXME:Generic Template Element",
    " <<user@hostname.org>>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_factory));
}

/* initialize the new element
 * instantiate pads and add them to element
 * set pad calback functions
 * initialize instance structure
 */
static void
gst_my_filter_init (GstMyFilter * filter)
{
  filter->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_event_function (filter->sinkpad,
                              GST_DEBUG_FUNCPTR(gst_my_filter_sink_event));
  gst_pad_set_chain_function (filter->sinkpad,
                              GST_DEBUG_FUNCPTR(gst_my_filter_chain));

  //gst_pad_set_query_function (filter->sinkpad, gst_base_src_query);

  GST_PAD_SET_PROXY_CAPS (filter->sinkpad);
  gst_element_add_pad (GST_ELEMENT (filter), filter->sinkpad);

  filter->srcpad = gst_pad_new_from_static_template (&src_factory, "src");
  //gst_pad_set_query_function (filter->srcpad, gst_base_src_query);
  GST_PAD_SET_PROXY_CAPS (filter->srcpad);
  gst_element_add_pad (GST_ELEMENT (filter), filter->srcpad);

  filter->silent = FALSE;


  //rundecklink();






}

static void
gst_my_filter_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstMyFilter *filter = GST_MYFILTER (object);

  switch (prop_id) {
    case PROP_SILENT:
      filter->silent = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_my_filter_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstMyFilter *filter = GST_MYFILTER (object);

  switch (prop_id) {
    case PROP_SILENT:
      g_value_set_boolean (value, filter->silent);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* GstElement vmethod implementations */

/* this function handles sink events */
static gboolean
gst_my_filter_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  gboolean ret;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps * caps;

      gst_event_parse_caps (event, &caps);
      /* do something with the caps */

      /* and forward */
      ret = gst_pad_event_default (pad, parent, event);
      break;
    }
    default:
      ret = gst_pad_event_default (pad, parent, event);
      break;
  }
  return ret;
}

/* chain function
 * this function does the actual processing
 */
static GstFlowReturn
gst_my_filter_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstMyFilter *filter = GST_MYFILTER (parent);
  gsize bufsize = gst_buffer_get_size (buf);
  GstClock *testclock = gst_element_get_clock ((GstElement*)filter);

  //GstClockTime pts = GST_BUFFER_PTS(buf);
  //pts = pts + 10000000000;
  //GST_BUFFER_PTS(buf) = pts;
  //GST_BUFFER_DTS(buf) = pts;

  if (filter->silent == FALSE) {
      /*
      GST_ELEMENT_WARNING (parent, CORE, CLOCK, (NULL),
              ("%s: bufsize:%ld, pts:%ld, dts:%ld, ts:%ld, d:%ld | %ld, %ld\n", 
              gst_element_get_name(parent),
              bufsize, 
              GST_BUFFER_PTS(buf), GST_BUFFER_DTS(buf), 
              GST_BUFFER_TIMESTAMP(buf), GST_BUFFER_DURATION(buf),
              gst_clock_get_time(testclock), gst_element_get_base_time((GstElement*)filter)));
              */

      g_print ("%s: bufsize:%ld, pts:%ld, dts:%ld, ts:%ld, d:%ld | %ld, %ld\n", 
              gst_element_get_name(parent),
              bufsize, 
              GST_BUFFER_PTS(buf), GST_BUFFER_DTS(buf), 
              GST_BUFFER_TIMESTAMP(buf), GST_BUFFER_DURATION(buf),
              gst_clock_get_time(testclock), gst_element_get_base_time((GstElement*)filter));
  }

  //exit(0);
  //
  {
      GstQuery *query = gst_query_new_latency();
      gboolean live = FALSE;
      GstClockTime min_latency = 0;
      GstClockTime max_latency = 0;

      gboolean res = gst_pad_peer_query (pad, query);
      if (res) {
          gst_query_parse_latency (query, &live, &min_latency, &max_latency);
          printf("\tmyfilter [%s] live:%d min:%ld max:%ld\n", gst_pad_get_name(pad), live, 
                  min_latency, max_latency);
      } else {
          printf("\tmyfilter [%s] query latency failed\n", gst_pad_get_name(pad));
      }
      res = gst_pad_peer_query (filter->srcpad, query);
      if (res) {
          gst_query_parse_latency (query, &live, &min_latency, &max_latency);
          printf("\tmyfilter [%s] live:%d min:%ld max:%ld\n", gst_pad_get_name(filter->srcpad), live, 
                  min_latency, max_latency);
      }

  }




  /* just push out the incoming buffer without touching it */
  return gst_pad_push (filter->srcpad, buf);
}

static gboolean
gst_base_src_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
    switch (GST_QUERY_TYPE (query)) {
        case GST_QUERY_LATENCY:
            {
                printf("[%s] query latency\n", gst_pad_get_name(pad));
                gst_query_set_latency(query, TRUE, 0, -1); // todo: what should we report?
            }
            break;
        default:
            gst_pad_query_default(pad, parent, query);
            
    }
    return true;
}


/*
    static gboolean
myfilter_init (GstPlugin * myfilter)
{
    GST_DEBUG_CATEGORY_INIT (gst_my_filter_debug, "myfilter",
            0, "Template myfilter");

    gst_element_register (myfilter, "myfilter", GST_RANK_NONE, GST_TYPE_MYFILTER);
    gst_element_register (myfilter, "tfidecklinksrc", GST_RANK_NONE, TFI_TYPE_DECKLINK_SRC);
    return TRUE;
}

#ifndef PACKAGE
#define PACKAGE "myfirstmyfilter"
#endif

GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    myfilter,
    "Template myfilter",
    myfilter_init,
    VERSION,
    "LGPL",
    "GStreamer",
    "http://gstreamer.net/"
)
*/
