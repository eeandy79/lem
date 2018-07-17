#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "tfisdisource.h"
#include "gstmyfilter.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static GstElementClass *parent_class = NULL;
GST_DEBUG_CATEGORY_STATIC (tfi_sdi_src_debug);
#define GST_CAT_DEFAULT tfi_sdi_src_debug
#define DEFAULT_IS_LIVE TRUE

#define _do_init \
    GST_DEBUG_CATEGORY_INIT (tfi_sdi_src_debug, "tfi_sdi_src", 0, "TFI SDI source plugin");
G_DEFINE_TYPE_WITH_CODE (TfiSdiSrc, tfi_sdi_src, GST_TYPE_MY_BASE_SRC, _do_init);

static void gst_sdi_src_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void gst_sdi_src_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static GstCaps *tfi_sdi_src_src_fixate (MyBaseSrc *bsrc, GstCaps *caps);
static void tfi_sdi_src_get_times (MyBaseSrc *basesrc, GstBuffer *buffer, GstClockTime *start, GstClockTime *end);
static gboolean tfi_sdi_src_decide_allocation (MyBaseSrc *bsrc, GstQuery *query);
static GstFlowReturn tfi_sdi_src_fill (MyBaseSrc *bsrc, guint64 offset, guint length, GstBuffer * ret);
static gboolean tfi_sdi_src_query (MyBaseSrc * src, GstQuery * query);
static GstFlowReturn tfi_sdi_src_alloc (MyBaseSrc * src, guint64 offset, guint size, GstBuffer ** buffer);

static GstFlowReturn create2 (GstPad *pad, GstBuffer ** buffer);
static void* work (void *p);
static void* workpad (void *p);
static void ready (MyBaseSrc *basesrc);
static GstPad *request_new_pad (GstElement *element, GstPadTemplate *template, const gchar *name, const GstCaps *caps);
static GstStateChangeReturn gst_base_src_change_state (GstElement * element,
    GstStateChange transition);

static GstPad *
request_new_pad (GstElement *element, GstPadTemplate *template, const gchar *name, const GstCaps *caps)
{
    gchar *pad_name;
    GstPad *pad;
    MyBaseSrc *basesrc = GST_MY_BASE_SRC_CAST (element);

    pad_name = g_strdup_printf ("src%d", basesrc->pad_counter++);
    pad = gst_pad_new_from_template (template, pad_name);
    g_free(pad_name);

    gst_element_add_pad (GST_ELEMENT (basesrc), pad);
    g_queue_push_tail(&basesrc->pad_queue, pad);

    return pad;
}


static void
tfi_sdi_src_class_init (TfiSdiSrcClass * klass)
{
    GObjectClass *gobject_class;
    GstElementClass *gstelement_class;
    MyBaseSrcClass *gstbasesrc_class;

    gobject_class = (GObjectClass *) klass;
    gstelement_class = (GstElementClass *) klass;
    gstbasesrc_class = (MyBaseSrcClass *) klass;
    parent_class = g_type_class_peek_parent (klass);

    gstelement_class->request_new_pad = request_new_pad;
    gstelement_class->change_state = gst_base_src_change_state;

    gobject_class->set_property = gst_sdi_src_set_property;
    gobject_class->get_property = gst_sdi_src_get_property;

    gst_element_class_set_static_metadata (gstelement_class,
            "TFI SDI source", "Source/Video",
            "Integrate with Blackmagic SDI card", "Andy Chang <andy.chang@tfidm.com>");

    gstbasesrc_class->get_caps = NULL;
    gstbasesrc_class->negotiate = NULL;
    gstbasesrc_class->event = NULL;
    gstbasesrc_class->alloc = tfi_sdi_src_alloc; // new test
    gstbasesrc_class->decide_allocation = NULL;
    gstbasesrc_class->create2 = create2;

    gstbasesrc_class->fixate = tfi_sdi_src_src_fixate;
    gstbasesrc_class->get_times = tfi_sdi_src_get_times;
    gstbasesrc_class->decide_allocation = tfi_sdi_src_decide_allocation;
    gstbasesrc_class->fill = tfi_sdi_src_fill;
    gstbasesrc_class->query = tfi_sdi_src_query;

    gstbasesrc_class->ready = ready;
}

static GstFlowReturn
tfi_sdi_src_alloc (MyBaseSrc * src, guint64 offset, guint size, GstBuffer ** buffer)
{
    GstMemory *memory;
    *buffer = gst_buffer_new();
    memory = gst_allocator_alloc(NULL, size, NULL);
    gst_buffer_insert_memory(*buffer, -1, memory);
    return GST_FLOW_OK;
}

static void
tfi_sdi_src_init (TfiSdiSrc *src)
{
    gst_base_src_set_format (GST_MY_BASE_SRC (src), GST_FORMAT_TIME);
}

static GstCaps *
tfi_sdi_src_src_fixate (MyBaseSrc *bsrc, GstCaps *caps)
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
tfi_sdi_src_decide_allocation (MyBaseSrc *bsrc, GstQuery *query)
{
    return TRUE;
}

static void
tfi_sdi_src_get_times (MyBaseSrc *basesrc, GstBuffer *buffer, GstClockTime *start, GstClockTime *end)
{
    GstClockTime timestamp = GST_BUFFER_PTS (buffer);
    if (GST_CLOCK_TIME_IS_VALID (timestamp)) {
        GstClockTime duration = GST_BUFFER_DURATION (buffer);
        if (GST_CLOCK_TIME_IS_VALID (duration)) {
            *end = timestamp + duration;
        }
        *start = timestamp;
    }
}

static GstFlowReturn
tfi_sdi_src_fill (MyBaseSrc *bsrc, guint64 offset, guint length, GstBuffer *ret)
{
    return GST_FLOW_OK;
}

static gboolean 
tfi_sdi_src_query (MyBaseSrc * src, GstQuery * query)
{
    gboolean res = FALSE;
    switch (GST_QUERY_TYPE (query)) {
        case GST_QUERY_CAPS:
            {
                MyBaseSrcClass *bclass;
                GstCaps *caps, *filter;

                bclass = GST_MY_BASE_SRC_GET_CLASS (src);
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
        case GST_QUERY_POSITION:
        case GST_QUERY_DURATION:
        case GST_QUERY_LATENCY:
        case GST_QUERY_JITTER:
        case GST_QUERY_RATE:
        case GST_QUERY_SEEKING:
        case GST_QUERY_SEGMENT:
        case GST_QUERY_CONVERT:
        case GST_QUERY_FORMATS:
        case GST_QUERY_BUFFERING:
        case GST_QUERY_CUSTOM:
        case GST_QUERY_URI:
        case GST_QUERY_ALLOCATION:
        case GST_QUERY_SCHEDULING:
        case GST_QUERY_ACCEPT_CAPS:
        case GST_QUERY_DRAIN:
        case GST_QUERY_CONTEXT:
        case GST_QUERY_UNKNOWN:
            res = FALSE;
    }
    return res;
}

static GstFlowReturn create2 (GstPad *pad, GstBuffer ** buffer)
{
    GstFlowReturn ret;
    int blocksize = ((long int)pad) & 0x00ffffff;
    MyBaseSrc *src = GST_MY_BASE_SRC (GST_OBJECT_PARENT (pad));
    MyBaseSrcClass *bclass = GST_MY_BASE_SRC_GET_CLASS (src);
    ret = bclass->create(src, 0, blocksize, buffer);
    GST_BUFFER_PTS (*buffer) = 10;
    GST_BUFFER_DTS (*buffer) = 11;
    g_print("%p:%d\n", pad, blocksize);
    return ret;
}

static void* workpad (void *p) 
{
    GstBuffer *buffer = NULL;
    gboolean isBuf;
    GstPad *pad = (GstPad*)p;
    create2(pad, &buffer);
    isBuf = GST_IS_BUFFER(buffer);
    g_print("isBuf_fuck:%d\n", isBuf);
    gst_pad_push(pad, buffer);
    return NULL;
}

static void* work (void *p) {
    MyBaseSrc *basesrc = (MyBaseSrc*)p;
    while(1) {
        g_print("\nrunning\n");
        gint i;
        gint length = g_queue_get_length(&basesrc->pad_queue);
        for (i = 0; i < length; i++) {
            GstPad *pad = g_queue_peek_nth (&basesrc->pad_queue, i);
            g_thread_new("padthread", &workpad, pad);
        }
        sleep(1);
    }
    return NULL;
}

static void ready (MyBaseSrc *basesrc) {
    TfiSdiSrc *src = (TfiSdiSrc*)basesrc;
    src->datathread = g_thread_new("datathread", &work, basesrc);
}

static GstStateChangeReturn
gst_base_src_change_state (GstElement * element, GstStateChange transition)
{
    MyBaseSrc *basesrc = GST_MY_BASE_SRC (element);
    MyBaseSrcClass *bclass = GST_MY_BASE_SRC_GET_CLASS (basesrc);
    GstStateChangeReturn result;

    switch (transition) {
        case GST_STATE_CHANGE_NULL_TO_READY:
            g_print("TFI:GST_STATE_CHANGE_NULL_TO_READY\n");
            break;
        case GST_STATE_CHANGE_READY_TO_PAUSED:
            g_print("TFI:GST_STATE_CHANGE_READY_TO_PAUSED\n");
            break;
        case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
            g_print("TFI:GST_STATE_CHANGE_PAUSED_TO_PLAYING\n");
            bclass->ready(basesrc);
            break;
        default:
            break;
    }

    if ((result = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition)) 
            == GST_STATE_CHANGE_FAILURE)
        goto failure;

    switch (transition) {
        case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
            break;
        case GST_STATE_CHANGE_PAUSED_TO_READY:
            break;
        case GST_STATE_CHANGE_READY_TO_NULL:
            break;
        default:
            break;
    }

    if (result == GST_STATE_CHANGE_SUCCESS)
        result = GST_STATE_CHANGE_NO_PREROLL;

    return result;

failure:
    {
        GST_DEBUG_OBJECT (basesrc, "parent failed state change");
        return result;
    }
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
