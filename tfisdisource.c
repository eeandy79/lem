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
G_DEFINE_TYPE_WITH_CODE (TfiSdiSrc, tfi_sdi_src, GST_TYPE_ELEMENT, _do_init);

static void gst_sdi_src_set_property (
        GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void gst_sdi_src_get_property (
        GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static GstStateChangeReturn gst_base_src_change_state (
        GstElement * element, GstStateChange transition);
static void gst_base_src_loop (GstPad * pad);

static void
tfi_sdi_src_class_init (TfiSdiSrcClass * klass)
{
    GObjectClass *gobject_class;
    GstElementClass *gstelement_class;

    gobject_class = (GObjectClass *) klass;
    gobject_class->set_property = gst_sdi_src_set_property;
    gobject_class->get_property = gst_sdi_src_get_property;

    gstelement_class = (GstElementClass *) klass;
    gstelement_class->change_state = GST_DEBUG_FUNCPTR (gst_base_src_change_state);

    gst_element_class_set_static_metadata (gstelement_class,
            "TFI SDI source", "Source/Video",
            "Integrate with Blackmagic SDI card", "Andy Chang <andy.chang@tfidm.com>");

    //gst_element_class_add_static_pad_template (gstelement_class, &video_src_template);
}

static void
tfi_sdi_src_init (TfiSdiSrc *src)
{
    src->stream_start_pending = TRUE;
    src->srcpad = gst_pad_new_from_static_template (&video_src_template, "src");
    gst_element_add_pad (GST_ELEMENT (src), src->srcpad);
}

static void
gst_sdi_src_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
}

static void
gst_sdi_src_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
}

static GstStateChangeReturn
gst_base_src_change_state (GstElement * element, GstStateChange transition)
{
    TfiSdiSrc *src;
    src = TFI_SDI_SRC (element);

    g_print("change state\n");

    switch (transition) {
        case GST_STATE_CHANGE_NULL_TO_READY:
            g_print("GST_STATE_CHANGE_NULL_TO_READY\n");
            break;
        case GST_STATE_CHANGE_READY_TO_PAUSED:
            g_print("GST_STATE_CHANGE_READY_TO_PAUSED\n");
            break;
        case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
            g_print("GST_STATE_CHANGE_PAUSED_TO_PLAYING\n");
            gst_pad_start_task (
                    src->srcpad, (GstTaskFunction) gst_base_src_loop,
                    src->srcpad, NULL);
            break;
        default:
            break;
    }
    //return GST_STATE_CHANGE_SUCCESS;
    return GST_STATE_CHANGE_NO_PREROLL;
}

static void
gst_base_src_loop (GstPad * pad)
{
    TfiSdiSrc *src;
    GstBuffer *buf = NULL;
    GstMemory *memory;

    src = TFI_SDI_SRC (GST_OBJECT_PARENT (pad));

    if (src->stream_start_pending == TRUE)
    {
        gchar *stream_id;
        GstEvent *event;

        stream_id = gst_pad_create_stream_id (src->srcpad, GST_ELEMENT_CAST (src), NULL);
        event = gst_event_new_stream_start (stream_id);
        gst_event_set_group_id (event, gst_util_group_id_next ());

        gst_pad_push_event (src->srcpad, event);
        g_free (stream_id);
        src->stream_start_pending = FALSE;
    }

    g_print("loop\n");
    buf = gst_buffer_new ();
    memory = gst_allocator_alloc (NULL, 1234, NULL);
    gst_buffer_insert_memory (buf, -1, memory);
    gst_pad_push (pad, buf);
    sleep(1);
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
