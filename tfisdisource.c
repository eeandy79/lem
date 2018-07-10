#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "tfisdisource.h"
#include "gstmyfilter.h"
#include <string.h>
#include <stdlib.h>

GST_DEBUG_CATEGORY_STATIC (tfi_sdi_src_debug);
#define GST_CAT_DEFAULT tfi_sdi_src_debug
#define DEFAULT_IS_LIVE TRUE

#define GST_LIVE_GET_LOCK(elem)               (&GST_BASE_SRC_CAST(elem)->live_lock)
#define GST_LIVE_LOCK(elem)                   g_mutex_lock(GST_LIVE_GET_LOCK(elem))
#define GST_LIVE_TRYLOCK(elem)                g_mutex_trylock(GST_LIVE_GET_LOCK(elem))
#define GST_LIVE_UNLOCK(elem)                 g_mutex_unlock(GST_LIVE_GET_LOCK(elem))
#define GST_LIVE_GET_COND(elem)               (&GST_BASE_SRC_CAST(elem)->live_cond)
#define GST_LIVE_WAIT(elem)                   g_cond_wait (GST_LIVE_GET_COND (elem), GST_LIVE_GET_LOCK (elem))
#define GST_LIVE_WAIT_UNTIL(elem, end_time)   g_cond_timed_wait (GST_LIVE_GET_COND (elem), GST_LIVE_GET_LOCK (elem), end_time)
#define GST_LIVE_SIGNAL(elem)                 g_cond_signal (GST_LIVE_GET_COND (elem));
#define GST_LIVE_BROADCAST(elem)              g_cond_broadcast (GST_LIVE_GET_COND (elem));

static GstStaticPadTemplate video_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
        GST_PAD_SRC,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS_ANY
        );

/*
static GstStaticPadTemplate audio_src_template =
GST_STATIC_PAD_TEMPLATE ("audio%u",
        GST_PAD_SRC,
        GST_PAD_SOMETIMES,
        GST_STATIC_CAPS_ANY
        );
        */

#define _do_init \
    GST_DEBUG_CATEGORY_INIT (tfi_sdi_src_debug, "tfi_sdi_src", 0, "TFI SDI source plugin");
G_DEFINE_TYPE_WITH_CODE (TfiSdiSrc, tfi_sdi_src, GST_TYPE_BASE_SRC, _do_init);

static void gst_sdi_src_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void gst_sdi_src_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static GstCaps *tfi_sdi_src_src_fixate (GstBaseSrc *bsrc, GstCaps *caps);
static void tfi_sdi_src_get_times (GstBaseSrc *basesrc, GstBuffer *buffer, GstClockTime *start, GstClockTime *end);
static gboolean tfi_sdi_src_decide_allocation (GstBaseSrc *bsrc, GstQuery *query);
static GstFlowReturn tfi_sdi_src_fill (GstBaseSrc *bsrc, guint64 offset, guint length, GstBuffer * ret);
static gboolean tfi_sdi_src_start (GstBaseSrc *basesrc);
static gboolean tfi_sdi_src_stop (GstBaseSrc *basesrc);
static gboolean tfi_sdi_src_query (GstBaseSrc * src, GstQuery * query);
static GstFlowReturn tfi_sdi_src_alloc (GstBaseSrc * src, guint64 offset,
    guint size, GstBuffer ** buffer);
static GstFlowReturn
tfi_sdi_src_create (GstBaseSrc * src, guint64 offset, guint size, GstBuffer ** buffer);
static gboolean
tfi_sdi_src_do_seek (GstBaseSrc * src, GstSegment * segment);
static GstStateChangeReturn
tfi_sdi_src_change_state (GstElement * element, GstStateChange transition);
static gboolean
tfi_sdi_src_set_playing (GstBaseSrc * basesrc, gboolean live_play);
static void
gst_base_src_loop (GstPad * pad);

static void
tfi_sdi_src_class_init (TfiSdiSrcClass * klass)
{
    GObjectClass *gobject_class;
    GstElementClass *gstelement_class;
    GstBaseSrcClass *gstbasesrc_class;

    gobject_class = (GObjectClass *) klass;
    gstelement_class = (GstElementClass *) klass;
    gstbasesrc_class = (GstBaseSrcClass *) klass;

    gobject_class->set_property = gst_sdi_src_set_property;
    gobject_class->get_property = gst_sdi_src_get_property;

    gst_element_class_set_static_metadata (gstelement_class,
            "TFI SDI source", "Source/Video",
            "Integrate with Blackmagic SDI card", "Andy Chang <andy.chang@tfidm.com>");

    gst_element_class_add_static_pad_template (gstelement_class, &video_src_template);
    //gst_element_class_add_static_pad_template (gstelement_class, &audio_src_template);

/*
    klass->get_caps = GST_DEBUG_FUNCPTR (gst_base_src_default_get_caps);
    klass->negotiate = GST_DEBUG_FUNCPTR (gst_base_src_default_negotiate);
    klass->prepare_seek_segment = GST_DEBUG_FUNCPTR (gst_base_src_default_prepare_seek_segment);
    klass->do_seek = GST_DEBUG_FUNCPTR (gst_base_src_default_do_seek);
    klass->event = GST_DEBUG_FUNCPTR (gst_base_src_default_event);
    klass->create = GST_DEBUG_FUNCPTR (gst_base_src_default_create);
    klass->alloc = GST_DEBUG_FUNCPTR (gst_base_src_default_alloc);
    klass->decide_allocation = GST_DEBUG_FUNCPTR (gst_base_src_decide_allocation_default);
*/

    gstbasesrc_class->get_caps = NULL;
    gstbasesrc_class->negotiate = NULL;
    gstbasesrc_class->prepare_seek_segment = NULL;
    gstbasesrc_class->do_seek = tfi_sdi_src_do_seek; // new test
    gstbasesrc_class->event = NULL;
    gstbasesrc_class->create = tfi_sdi_src_create; // new test
    gstbasesrc_class->alloc = tfi_sdi_src_alloc; // new test
    gstbasesrc_class->decide_allocation = NULL;

    //gstelement_class->change_state = tfi_sdi_src_change_state;


    gstbasesrc_class->fixate = tfi_sdi_src_src_fixate;
    gstbasesrc_class->get_times = tfi_sdi_src_get_times;
    gstbasesrc_class->start = tfi_sdi_src_start;
    gstbasesrc_class->stop = tfi_sdi_src_stop;
    gstbasesrc_class->decide_allocation = tfi_sdi_src_decide_allocation;
    gstbasesrc_class->fill = tfi_sdi_src_fill;
    gstbasesrc_class->query = tfi_sdi_src_query;
}

static GstFlowReturn
tfi_sdi_src_alloc (GstBaseSrc * src, guint64 offset, guint size, GstBuffer ** buffer)
{
    GstMemory *memory;
    *buffer = gst_buffer_new();
    memory = gst_allocator_alloc(NULL, 12345, NULL);
    gst_buffer_insert_memory(*buffer, -1, memory);
    return GST_FLOW_OK;
}

static void
tfi_sdi_src_init (TfiSdiSrc *src)
{
    gst_base_src_set_format (GST_BASE_SRC (src), GST_FORMAT_TIME);
    gst_base_src_set_live (GST_BASE_SRC (src), DEFAULT_IS_LIVE);
}

static GstCaps *
tfi_sdi_src_src_fixate (GstBaseSrc *bsrc, GstCaps *caps)
{
    return NULL;
}

static void
gst_sdi_src_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
}

static void
gst_sdi_src_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
}

static gboolean
tfi_sdi_src_decide_allocation (GstBaseSrc *bsrc, GstQuery *query)
{
    return TRUE;
}

static void
tfi_sdi_src_get_times (GstBaseSrc *basesrc, GstBuffer *buffer, GstClockTime *start, GstClockTime *end)
{
    /* for live sources, sync on the timestamp of the buffer */
    if (gst_base_src_is_live (basesrc)) {
        GstClockTime timestamp = GST_BUFFER_PTS (buffer);

        //g_print("is live:%lld\n", timestamp);
        if (GST_CLOCK_TIME_IS_VALID (timestamp)) {
            /* get duration to calculate end time */
            GstClockTime duration = GST_BUFFER_DURATION (buffer);

            if (GST_CLOCK_TIME_IS_VALID (duration)) {
                *end = timestamp + duration;
            }
            *start = timestamp;
        }
    } else {
        g_print("is file\n");
        *start = -1;
        *end = -1;
    }
}

static GstFlowReturn
tfi_sdi_src_fill (GstBaseSrc *bsrc, guint64 offset, guint length, GstBuffer *ret)
{
/*
    //exit(0);
    gboolean start = (GST_PAD_MODE (bsrc->srcpad) == GST_PAD_MODE_PUSH);
    g_print("buffersize: %ld,%d,%p,%d\n", offset, length, ret, start);
*/
    return GST_FLOW_OK;
}

static gboolean
tfi_sdi_src_start (GstBaseSrc *basesrc)
{
    return TRUE;
}

static gboolean
tfi_sdi_src_stop (GstBaseSrc *basesrc)
{
    return TRUE;
}

static gboolean 
tfi_sdi_src_query (GstBaseSrc * src, GstQuery * query)
{
    gboolean res;
    switch (GST_QUERY_TYPE (query)) {
        case GST_QUERY_CAPS:
            {
                GstBaseSrcClass *bclass;
                GstCaps *caps, *filter;

                bclass = GST_BASE_SRC_GET_CLASS (src);
                if (bclass->get_caps) {
                    gst_query_parse_caps (query, &filter);
                    if ((caps = bclass->get_caps (src, filter))) {
                        gst_query_set_caps_result (query, caps);
                        gst_caps_unref (caps);
                        res = TRUE;
                    } else {
                        res = FALSE;
                    }
                } else
                    res = FALSE;
                break;
            }
    }
    return res;
}

static GstFlowReturn
tfi_sdi_src_create (GstBaseSrc * src, guint64 offset, guint size, GstBuffer ** buffer)
{
  GstBaseSrcClass *bclass;
  GstFlowReturn ret;
  GstBuffer *res_buf;

  bclass = GST_BASE_SRC_GET_CLASS (src);
  if (G_UNLIKELY (!bclass->alloc))
    goto no_function;
  if (G_UNLIKELY (!bclass->fill))
    goto no_function;

  if (*buffer == NULL) {
    ret = bclass->alloc (src, offset, size, &res_buf);
    if (G_UNLIKELY (ret != GST_FLOW_OK))
      goto alloc_failed;
  } else {
    res_buf = *buffer;
  }

  if (G_LIKELY (size > 0)) {
    /* only call fill when there is a size */
    ret = bclass->fill (src, offset, size, res_buf);
    if (G_UNLIKELY (ret != GST_FLOW_OK))
      goto not_ok;
  }

  *buffer = res_buf;

  return GST_FLOW_OK;

  /* ERRORS */
no_function:
  {
    GST_DEBUG_OBJECT (src, "no fill or alloc function");
    return GST_FLOW_NOT_SUPPORTED;
  }
alloc_failed:
  {
    GST_DEBUG_OBJECT (src, "Failed to allocate buffer of %u bytes", size);
    return ret;
  }
not_ok:
  {
    GST_DEBUG_OBJECT (src, "fill returned %d (%s)", ret,
        gst_flow_get_name (ret));
    if (*buffer == NULL)
      gst_buffer_unref (res_buf);
    return ret;
  }
}

static gboolean
tfi_sdi_src_do_seek (GstBaseSrc * src, GstSegment * segment)
{
  return TRUE;
}

static GstStateChangeReturn
tfi_sdi_src_change_state (GstElement * element, GstStateChange transition)
{
    GstBaseSrc *basesrc;
    basesrc = GST_BASE_SRC (element);

    switch (transition) {
        case GST_STATE_CHANGE_NULL_TO_READY:
            break;
        case GST_STATE_CHANGE_READY_TO_PAUSED:
            break;
        case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
            {
                tfi_sdi_src_set_playing(basesrc, TRUE);
            }
/*
            if (gst_base_src_is_live (basesrc)) {
                gst_base_src_set_playing (basesrc, TRUE);
            }
*/
            break;
        default:
            break;
    }
    return GST_STATE_CHANGE_NO_PREROLL;
}

static gboolean
tfi_sdi_src_set_playing (GstBaseSrc * basesrc, gboolean live_play)
{
    /* we are now able to grab the LIVE lock, when we get it, we can be
     *    * waiting for PLAYING while blocked in the LIVE cond or we can be waiting
     *       * for the clock. */
    GST_LIVE_LOCK (basesrc);
    GST_DEBUG_OBJECT (basesrc, "unschedule clock");

    /* unblock clock sync (if any) */
    if (basesrc->clock_id)
        gst_clock_id_unschedule (basesrc->clock_id);

    /* configure what to do when we get to the LIVE lock. */
    GST_DEBUG_OBJECT (basesrc, "live running %d", live_play);
    basesrc->live_running = live_play;

    if (live_play) {
        gboolean start;

        /* for live sources we restart the timestamp correction */
        GST_OBJECT_LOCK (basesrc);
        // basesrc->priv->latency = -1;
        GST_OBJECT_UNLOCK (basesrc);
        /* have to restart the task in case it stopped because of the unlock when
         *      * we went to PAUSED. Only do this if we operating in push mode. */
        GST_OBJECT_LOCK (basesrc->srcpad);
        start = (GST_PAD_MODE (basesrc->srcpad) == GST_PAD_MODE_PUSH);
        start = TRUE;
        g_print("start:%d\n", start);
        GST_OBJECT_UNLOCK (basesrc->srcpad);
        if (start) {
            g_print("good start\n");
            gst_pad_start_task (basesrc->srcpad, (GstTaskFunction) gst_base_src_loop,
                    basesrc->srcpad, NULL);
        }
        GST_DEBUG_OBJECT (basesrc, "signal");
        GST_LIVE_SIGNAL (basesrc);
    }
    GST_LIVE_UNLOCK (basesrc);

    return TRUE;
}

static gboolean stream_start_pending = TRUE;

static void
gst_base_src_loop (GstPad * pad)
{
    GstBuffer *buffer = NULL;
    GstBaseSrc *src;
    src = GST_BASE_SRC (GST_OBJECT_PARENT (pad));

    //if (src->priv->stream_start_pending) {
    if (stream_start_pending) {
        gchar *stream_id;
        GstEvent *event;

        stream_id =
            gst_pad_create_stream_id (src->srcpad, GST_ELEMENT_CAST (src), NULL);

        GST_DEBUG_OBJECT (src, "Pushing STREAM_START");
        g_print("\t\t\tPushing STREAM_START");
        event = gst_event_new_stream_start (stream_id);
        gst_event_set_group_id (event, gst_util_group_id_next ());

        gst_pad_push_event (src->srcpad, event);
        //src->priv->stream_start_pending = FALSE;
        stream_start_pending = FALSE;
        g_free (stream_id);

        gst_pad_mark_reconfigure (pad);
    }

    tfi_sdi_src_create (src, 0, 22222, &buffer);
    gst_pad_push(pad, buffer);
}

static gboolean
plugin_init (GstPlugin *plugin)
{
    gst_element_register (plugin, "tfi_sdi_src", GST_RANK_NONE, GST_TYPE_TFI_SDI_SRC);
    gst_element_register (plugin, "myfilter", GST_RANK_NONE, GST_TYPE_MYFILTER);
    return TRUE;
}

GST_PLUGIN_DEFINE (
        GST_VERSION_MAJOR,
        GST_VERSION_MINOR,
        tfi_sdi_src,
        "TFI SDI source plugin",
        plugin_init, VERSION, "LGPL", "TFI", "http://www.tfidm.com/"
        )
