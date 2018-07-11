#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "tfisdisource.h"
#include "gstmyfilter.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

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
static gboolean tfi_sdi_src_start (MyBaseSrc *basesrc);
static gboolean tfi_sdi_src_stop (MyBaseSrc *basesrc);
static gboolean tfi_sdi_src_query (MyBaseSrc * src, GstQuery * query);
static GstFlowReturn tfi_sdi_src_alloc (MyBaseSrc * src, guint64 offset, guint size, GstBuffer ** buffer);

static GstFlowReturn create2 (GstPad *pad, GstBuffer ** buffer);

static void
tfi_sdi_src_class_init (TfiSdiSrcClass * klass)
{
    GObjectClass *gobject_class;
    GstElementClass *gstelement_class;
    MyBaseSrcClass *gstbasesrc_class;

    gobject_class = (GObjectClass *) klass;
    gstelement_class = (GstElementClass *) klass;
    gstbasesrc_class = (MyBaseSrcClass *) klass;

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
    gstbasesrc_class->start = tfi_sdi_src_start;
    gstbasesrc_class->stop = tfi_sdi_src_stop;
    gstbasesrc_class->decide_allocation = tfi_sdi_src_decide_allocation;
    gstbasesrc_class->fill = tfi_sdi_src_fill;
    gstbasesrc_class->query = tfi_sdi_src_query;
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
    gst_base_src_set_live (GST_MY_BASE_SRC (src), DEFAULT_IS_LIVE);
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
tfi_sdi_src_fill (MyBaseSrc *bsrc, guint64 offset, guint length, GstBuffer *ret)
{
    return GST_FLOW_OK;
}

static gboolean
tfi_sdi_src_start (MyBaseSrc *basesrc)
{
    return TRUE;
}

static gboolean
tfi_sdi_src_stop (MyBaseSrc *basesrc)
{
    return TRUE;
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
    int blocksize = ((int)pad) & 0x0000ffff;
    MyBaseSrc *src = GST_MY_BASE_SRC (GST_OBJECT_PARENT (pad));
    MyBaseSrcClass *bclass = GST_MY_BASE_SRC_GET_CLASS (src);
    return bclass->create(src, 0, blocksize, buffer);
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
