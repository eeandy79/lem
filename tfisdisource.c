#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "tfisdisource.h"
#include <string.h>
#include <stdlib.h>

GST_DEBUG_CATEGORY_STATIC (tfi_sdi_src_debug);
#define GST_CAT_DEFAULT tfi_sdi_src_debug
#define DEFAULT_IS_LIVE TRUE

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

    gstbasesrc_class->fixate = tfi_sdi_src_src_fixate;
    gstbasesrc_class->get_times = tfi_sdi_src_get_times;
    gstbasesrc_class->start = tfi_sdi_src_start;
    gstbasesrc_class->stop = tfi_sdi_src_stop;
    gstbasesrc_class->decide_allocation = tfi_sdi_src_decide_allocation;
    gstbasesrc_class->fill = tfi_sdi_src_fill;
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
    g_print("buffersize: %ld,%d,%p\n", offset, length, ret);
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
plugin_init (GstPlugin *plugin)
{
    return gst_element_register (plugin, "tfi_sdi_src", GST_RANK_NONE, GST_TYPE_TFI_SDI_SRC);
}

GST_PLUGIN_DEFINE (
        GST_VERSION_MAJOR,
        GST_VERSION_MINOR,
        tfi_sdi_src,
        "TFI SDI source plugin",
        plugin_init, VERSION, "LGPL", "TFI", "http://www.tfidm.com/"
        )
