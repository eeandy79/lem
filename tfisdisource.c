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
static GstFlowReturn tfi_sdi_src_alloc (MyBaseSrc * src, guint64 offset, guint size, GstBuffer ** buffer);

static GstFlowReturn create2 (GstPad *pad, GstBuffer ** buffer);
static void* work (void *p);
static void* workpad (void *p);
static void ready (TfiSdiSrc *src);
static GstPad *request_new_pad (GstElement *element, GstPadTemplate *template, const gchar *name, const GstCaps *caps);
static GstStateChangeReturn gst_base_src_change_state (GstElement * element,
    GstStateChange transition);

static GstPad *
request_new_pad (GstElement *element, GstPadTemplate *template, const gchar *name, const GstCaps *caps)
{
    gchar *pad_name;
    GstPad *pad;
    TfiSdiSrc *src = (TfiSdiSrc*)element;
    
    pad_name = g_strdup_printf ("src%d", src->pad_counter++);
    pad = gst_pad_new_from_template (template, pad_name);
    g_free(pad_name);

    gst_element_add_pad (GST_ELEMENT (src), pad);
    g_queue_push_tail(&src->pad_queue, pad);

    return pad;
}


static void
tfi_sdi_src_class_init (TfiSdiSrcClass * klass)
{
    GObjectClass *gobject_class;
    GstElementClass *gstelement_class;

    gobject_class = (GObjectClass *) klass;
    gstelement_class = (GstElementClass *) klass;
    parent_class = g_type_class_peek_parent (klass);

    gstelement_class->request_new_pad = request_new_pad;
    gstelement_class->change_state = gst_base_src_change_state;

    gobject_class->set_property = gst_sdi_src_set_property;
    gobject_class->get_property = gst_sdi_src_get_property;

    gst_element_class_set_static_metadata (gstelement_class,
            "TFI SDI source", "Source/Video",
            "Integrate with Blackmagic SDI card", "Andy Chang <andy.chang@tfidm.com>");
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
    src->pad_counter = 0;

    g_queue_init(&src->pad_queue);
}

static void
gst_sdi_src_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
}

static void
gst_sdi_src_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
}

static GstFlowReturn create2 (GstPad *pad, GstBuffer ** buffer)
{
    GstFlowReturn ret;
    int blocksize = ((long int)pad) & 0x00ffffff;
    MyBaseSrc *src = GST_MY_BASE_SRC (GST_OBJECT_PARENT (pad));

    ret = tfi_sdi_src_alloc (src, 0, blocksize, buffer);

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
    TfiSdiSrc *basesrc = (TfiSdiSrc*)p;
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

static void ready (TfiSdiSrc *src) {
    src->datathread = g_thread_new("datathread", &work, src);
}

static GstStateChangeReturn
gst_base_src_change_state (GstElement * element, GstStateChange transition)
{
    TfiSdiSrc *src = TFI_SDI_SRC (element);
    GstStateChangeReturn result;
    gboolean no_preroll = FALSE;

    switch (transition) {
        case GST_STATE_CHANGE_NULL_TO_READY:
            g_print("TFI:GST_STATE_CHANGE_NULL_TO_READY\n");
            break;
        case GST_STATE_CHANGE_READY_TO_PAUSED:
            g_print("TFI:GST_STATE_CHANGE_READY_TO_PAUSED\n");
            no_preroll = TRUE;
            break;
        case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
            g_print("TFI:GST_STATE_CHANGE_PAUSED_TO_PLAYING\n");
            ready(src);
            break;
        default:
            break;
    }

    if ((result = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition)) 
            == GST_STATE_CHANGE_FAILURE)
        goto failure;

    switch (transition) {
        case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
            no_preroll = TRUE;
            break;
        case GST_STATE_CHANGE_PAUSED_TO_READY:
            break;
        case GST_STATE_CHANGE_READY_TO_NULL:
            break;
        default:
            break;
    }

    if (no_preroll && result == GST_STATE_CHANGE_SUCCESS)
        result = GST_STATE_CHANGE_NO_PREROLL;

    return result;

failure:
    {
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
