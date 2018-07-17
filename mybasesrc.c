#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

//#include <gst/gst_private.h>
//#include <gst/glib-compat-private.h>

#include "mybasesrc.h"
//#include <gst/gst-i18n-lib.h>

GST_DEBUG_CATEGORY_STATIC (gst_base_src_debug);
#define GST_CAT_DEFAULT gst_base_src_debug

#define GST_LIVE_GET_LOCK(elem)               (&GST_MY_BASE_SRC_CAST(elem)->live_lock)
#define GST_LIVE_LOCK(elem)                   g_mutex_lock(GST_LIVE_GET_LOCK(elem))
#define GST_LIVE_TRYLOCK(elem)                g_mutex_trylock(GST_LIVE_GET_LOCK(elem))
#define GST_LIVE_UNLOCK(elem)                 g_mutex_unlock(GST_LIVE_GET_LOCK(elem))
#define GST_LIVE_GET_COND(elem)               (&GST_MY_BASE_SRC_CAST(elem)->live_cond)
#define GST_LIVE_WAIT(elem)                   g_cond_wait (GST_LIVE_GET_COND (elem), GST_LIVE_GET_LOCK (elem))
#define GST_LIVE_WAIT_UNTIL(elem, end_time)   g_cond_timed_wait (GST_LIVE_GET_COND (elem), GST_LIVE_GET_LOCK (elem), end_time)
#define GST_LIVE_SIGNAL(elem)                 g_cond_signal (GST_LIVE_GET_COND (elem));
#define GST_LIVE_BROADCAST(elem)              g_cond_broadcast (GST_LIVE_GET_COND (elem));


#define GST_ASYNC_GET_COND(elem)              (&GST_MY_BASE_SRC_CAST(elem)->priv->async_cond)
#define GST_ASYNC_WAIT(elem)                  g_cond_wait (GST_ASYNC_GET_COND (elem), GST_OBJECT_GET_LOCK (elem))
#define GST_ASYNC_SIGNAL(elem)                g_cond_signal (GST_ASYNC_GET_COND (elem));

#define CLEAR_PENDING_EOS(bsrc) \
  G_STMT_START { \
    g_atomic_int_set (&bsrc->priv->has_pending_eos, FALSE); \
    gst_event_replace (&bsrc->priv->pending_eos, NULL); \
  } G_STMT_END


/* BaseSrc signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

#define DEFAULT_BLOCKSIZE       4096
#define DEFAULT_NUM_BUFFERS     -1
#define DEFAULT_DO_TIMESTAMP    FALSE

enum
{
  PROP_0,
  PROP_BLOCKSIZE,
  PROP_NUM_BUFFERS,
#ifndef GST_REMOVE_DEPRECATED
  PROP_TYPEFIND,
#endif
  PROP_DO_TIMESTAMP
};

/* The basesrc implementation need to respect the following locking order:
 *   1. STREAM_LOCK
 *   2. LIVE_LOCK
 *   3. OBJECT_LOCK
 */
struct _MyBaseSrcPrivate
{
  gboolean discont;             /* STREAM_LOCK */
  gboolean flushing;            /* LIVE_LOCK */

  GstFlowReturn start_result;   /* OBJECT_LOCK */
  gboolean async;               /* OBJECT_LOCK */

  /* if segment should be sent and a
   * seqnum if it was originated by a seek */
  gboolean segment_pending;     /* OBJECT_LOCK */
  guint32 segment_seqnum;       /* OBJECT_LOCK */

  /* if EOS is pending (atomic) */
  GstEvent *pending_eos;        /* OBJECT_LOCK */
  gint has_pending_eos;         /* atomic */

  /* if the eos was caused by a forced eos from the application */
  gboolean forced_eos;          /* LIVE_LOCK */

  /* startup latency is the time it takes between going to PLAYING and producing
   * the first BUFFER with running_time 0. This value is included in the latency
   * reporting. */
  GstClockTime latency;         /* OBJECT_LOCK */
  /* timestamp offset, this is the offset add to the values of gst_times for
   * pseudo live sources */
  GstClockTimeDiff ts_offset;   /* OBJECT_LOCK */

  gboolean do_timestamp;        /* OBJECT_LOCK */
  volatile gint dynamic_size;   /* atomic */
  volatile gint automatic_eos;  /* atomic */

  /* stream sequence number */
  guint32 seqnum;               /* STREAM_LOCK */

  /* pending events (TAG, CUSTOM_BOTH, CUSTOM_DOWNSTREAM) to be
   * pushed in the data stream */
  GList *pending_events;        /* OBJECT_LOCK */
  volatile gint have_events;    /* OBJECT_LOCK */

  /* QoS *//* with LOCK */
  gdouble proportion;           /* OBJECT_LOCK */
  GstClockTime earliest_time;   /* OBJECT_LOCK */

  GstBufferPool *pool;          /* OBJECT_LOCK */
  GstAllocator *allocator;      /* OBJECT_LOCK */
  GstAllocationParams params;   /* OBJECT_LOCK */

  GCond async_cond;             /* OBJECT_LOCK */

  /* for _submit_buffer_list() */
  GstBufferList *pending_bufferlist;
};

#define MY_BASE_SRC_HAS_PENDING_BUFFER_LIST(src) \
    ((src)->priv->pending_bufferlist != NULL)

static GstStaticPadTemplate tee_src_template = GST_STATIC_PAD_TEMPLATE ("src%d",
    GST_PAD_SRC,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS_ANY);

static gint private_offset = 0;

static void gst_base_src_class_init (MyBaseSrcClass * klass);
static void gst_base_src_init (MyBaseSrc * src, gpointer g_class);
static void gst_base_src_finalize (GObject * object);

GType
gst_base_src_get_type (void)
{
  static volatile gsize base_src_type = 0;

  if (g_once_init_enter (&base_src_type)) {
    GType _type;
    static const GTypeInfo base_src_info = {
      sizeof (MyBaseSrcClass),
      NULL,
      NULL,
      (GClassInitFunc) gst_base_src_class_init,
      NULL,
      NULL,
      sizeof (MyBaseSrc),
      0,
      (GInstanceInitFunc) gst_base_src_init,
    };

    _type = g_type_register_static (GST_TYPE_ELEMENT,
        "MyBaseSrc", &base_src_info, G_TYPE_FLAG_ABSTRACT);

    private_offset =
        g_type_add_instance_private (_type, sizeof (MyBaseSrcPrivate));

    g_once_init_leave (&base_src_type, _type);
  }
  return base_src_type;
}

static inline MyBaseSrcPrivate *
gst_base_src_get_instance_private (MyBaseSrc * self)
{
  return (G_STRUCT_MEMBER_P (self, private_offset));
}

static GstCaps *gst_base_src_default_get_caps (MyBaseSrc * bsrc, GstCaps * filter);
static GstCaps *gst_base_src_default_fixate (MyBaseSrc * src, GstCaps * caps);
static GstCaps *gst_base_src_fixate (MyBaseSrc * src, GstCaps * caps);

static gboolean gst_base_src_is_random_access (MyBaseSrc * src);
static void gst_base_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_base_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static gboolean gst_base_src_event (GstPad * pad, GstObject * parent,
    GstEvent * event);
static gboolean gst_base_src_send_event (GstElement * elem, GstEvent * event);
static gboolean gst_base_src_default_event (MyBaseSrc * src, GstEvent * event);

static gboolean gst_base_src_query (GstPad * pad, GstObject * parent,
    GstQuery * query);

static void gst_base_src_set_pool_flushing (MyBaseSrc * basesrc,
    gboolean flushing);
static gboolean gst_base_src_default_negotiate (MyBaseSrc * basesrc);
static gboolean gst_base_src_default_query (MyBaseSrc * src, GstQuery * query);
static GstFlowReturn gst_base_src_default_create (MyBaseSrc * basesrc,
    guint64 offset, guint size, GstBuffer ** buf);
static GstFlowReturn gst_base_src_default_alloc (MyBaseSrc * basesrc,
    guint64 offset, guint size, GstBuffer ** buf);
static gboolean gst_base_src_decide_allocation_default (MyBaseSrc * basesrc,
    GstQuery * query);

static gboolean gst_base_src_set_flushing (MyBaseSrc * basesrc,
    gboolean flushing);


static void gst_base_src_loop (GstPad * pad);
static GstFlowReturn gst_base_src_get_range2 (GstPad * pad, GstBuffer **buf);
static gboolean gst_base_src_negotiate (GstPad *pad);
static gboolean gst_base_src_update_length (MyBaseSrc * src, guint64 offset,
    guint * length, gboolean force);


static void
gst_base_src_class_init (MyBaseSrcClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gstelement_class = GST_ELEMENT_CLASS (klass);

  if (private_offset != 0)
    g_type_class_adjust_private_offset (klass, &private_offset);

  GST_DEBUG_CATEGORY_INIT (gst_base_src_debug, "basesrc", 0, "basesrc element");

  gobject_class->finalize = gst_base_src_finalize;
  gobject_class->set_property = gst_base_src_set_property;
  gobject_class->get_property = gst_base_src_get_property;

  g_object_class_install_property (gobject_class, PROP_BLOCKSIZE,
      g_param_spec_uint ("blocksize", "Block size",
          "Size in bytes to read per buffer (-1 = default)", 0, G_MAXUINT,
          DEFAULT_BLOCKSIZE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_NUM_BUFFERS,
      g_param_spec_int ("num-buffers", "num-buffers",
          "Number of buffers to output before sending EOS (-1 = unlimited)",
          -1, G_MAXINT, DEFAULT_NUM_BUFFERS, G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS));
#ifndef GST_REMOVE_DEPRECATED
  g_object_class_install_property (gobject_class, PROP_TYPEFIND,
      g_param_spec_boolean ("typefind", "Typefind",
          "Run typefind before negotiating (deprecated, non-functional)", FALSE,
          G_PARAM_READWRITE | G_PARAM_DEPRECATED | G_PARAM_STATIC_STRINGS));
#endif
  g_object_class_install_property (gobject_class, PROP_DO_TIMESTAMP,
      g_param_spec_boolean ("do-timestamp", "Do timestamp",
          "Apply current stream time to buffers", DEFAULT_DO_TIMESTAMP,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gstelement_class->send_event = GST_DEBUG_FUNCPTR (gst_base_src_send_event);

  klass->get_caps = GST_DEBUG_FUNCPTR (gst_base_src_default_get_caps);
  klass->negotiate = GST_DEBUG_FUNCPTR (gst_base_src_default_negotiate);
  klass->fixate = GST_DEBUG_FUNCPTR (gst_base_src_default_fixate);
  klass->query = GST_DEBUG_FUNCPTR (gst_base_src_default_query);
  klass->event = GST_DEBUG_FUNCPTR (gst_base_src_default_event);
  klass->create = GST_DEBUG_FUNCPTR (gst_base_src_default_create);
  klass->alloc = GST_DEBUG_FUNCPTR (gst_base_src_default_alloc);
  klass->decide_allocation = GST_DEBUG_FUNCPTR (gst_base_src_decide_allocation_default);

  GST_DEBUG_REGISTER_FUNCPTR (gst_base_src_event);
  GST_DEBUG_REGISTER_FUNCPTR (gst_base_src_query);
  GST_DEBUG_REGISTER_FUNCPTR (gst_base_src_fixate);
}

static void
gst_base_src_init (MyBaseSrc * basesrc, gpointer g_class)
{
  g_mutex_init (&basesrc->live_lock);
  g_cond_init (&basesrc->live_cond);

  basesrc->priv = gst_base_src_get_instance_private (basesrc);
  basesrc->num_buffers = DEFAULT_NUM_BUFFERS;
  basesrc->num_buffers_left = -1;
  basesrc->priv->automatic_eos = TRUE;
  basesrc->can_activate_push = TRUE;
  basesrc->blocksize = DEFAULT_BLOCKSIZE;
  basesrc->clock_id = NULL;

  gst_element_class_add_pad_template (GST_ELEMENT_CLASS (g_class),
        gst_static_pad_template_get (&tee_src_template));

  /* we operate in BYTES by default */
  gst_base_src_set_format (basesrc, GST_FORMAT_BYTES);
  basesrc->priv->do_timestamp = DEFAULT_DO_TIMESTAMP;
  g_atomic_int_set (&basesrc->priv->have_events, FALSE);

  g_cond_init (&basesrc->priv->async_cond);
  basesrc->priv->start_result = GST_FLOW_FLUSHING;
  GST_OBJECT_FLAG_UNSET (basesrc, GST_MY_BASE_SRC_FLAG_STARTED);
  GST_OBJECT_FLAG_UNSET (basesrc, GST_MY_BASE_SRC_FLAG_STARTING);
  GST_OBJECT_FLAG_SET (basesrc, GST_ELEMENT_FLAG_SOURCE);
}

static void
gst_base_src_finalize (GObject * object)
{
  MyBaseSrc *basesrc;
  GstEvent **event_p;

  basesrc = GST_MY_BASE_SRC (object);

  g_mutex_clear (&basesrc->live_lock);
  g_cond_clear (&basesrc->live_cond);
  g_cond_clear (&basesrc->priv->async_cond);

  event_p = &basesrc->pending_seek;
  gst_event_replace (event_p, NULL);

  if (basesrc->priv->pending_events) {
    g_list_foreach (basesrc->priv->pending_events, (GFunc) gst_event_unref,
        NULL);
    g_list_free (basesrc->priv->pending_events);
  }
}

/* Call with LIVE_LOCK held */
static GstFlowReturn
gst_base_src_wait_playing_unlocked (MyBaseSrc * src)
{
  while (G_UNLIKELY (!src->live_running && !src->priv->flushing)) {
    /* block until the state changes, or we get a flush, or something */
    GST_DEBUG_OBJECT (src, "live source waiting for running state");
    GST_LIVE_WAIT (src);
    GST_DEBUG_OBJECT (src, "live source unlocked");
  }

  if (src->priv->flushing)
    goto flushing;

  return GST_FLOW_OK;

  /* ERRORS */
flushing:
  {
    GST_DEBUG_OBJECT (src, "we are flushing");
    return GST_FLOW_FLUSHING;
  }
}


/**
 * gst_base_src_wait_playing:
 * @src: the src
 *
 * If the #MyBaseSrcClass.create() method performs its own synchronisation
 * against the clock it must unblock when going from PLAYING to the PAUSED state
 * and call this method before continuing to produce the remaining data.
 *
 * This function will block until a state change to PLAYING happens (in which
 * case this function returns %GST_FLOW_OK) or the processing must be stopped due
 * to a state change to READY or a FLUSH event (in which case this function
 * returns %GST_FLOW_FLUSHING).
 *
 * Returns: %GST_FLOW_OK if @src is PLAYING and processing can
 * continue. Any other return value should be returned from the create vmethod.
 */
GstFlowReturn
gst_base_src_wait_playing (MyBaseSrc * src)
{
  GstFlowReturn ret;

  g_return_val_if_fail (GST_IS_MY_BASE_SRC (src), GST_FLOW_ERROR);

  GST_LIVE_LOCK (src);
  ret = gst_base_src_wait_playing_unlocked (src);
  GST_LIVE_UNLOCK (src);

  return ret;
}

/**
 * gst_base_src_set_format:
 * @src: base source instance
 * @format: the format to use
 *
 * Sets the default format of the source. This will be the format used
 * for sending SEGMENT events and for performing seeks.
 *
 * If a format of GST_FORMAT_BYTES is set, the element will be able to
 * operate in pull mode if the #MyBaseSrcClass.is_seekable() returns %TRUE.
 *
 * This function must only be called in states < %GST_STATE_PAUSED.
 */
void
gst_base_src_set_format (MyBaseSrc * src, GstFormat format)
{
  g_return_if_fail (GST_IS_MY_BASE_SRC (src));
  g_return_if_fail (GST_STATE (src) <= GST_STATE_READY);

  GST_OBJECT_LOCK (src);
  gst_segment_init (&src->segment, format);
  GST_OBJECT_UNLOCK (src);
}

/**
 * gst_base_src_set_dynamic_size:
 * @src: base source instance
 * @dynamic: new dynamic size mode
 *
 * If not @dynamic, size is only updated when needed, such as when trying to
 * read past current tracked size.  Otherwise, size is checked for upon each
 * read.
 */
void
gst_base_src_set_dynamic_size (MyBaseSrc * src, gboolean dynamic)
{
  g_return_if_fail (GST_IS_MY_BASE_SRC (src));

  g_atomic_int_set (&src->priv->dynamic_size, dynamic);
}

/**
 * gst_base_src_set_automatic_eos:
 * @src: base source instance
 * @automatic_eos: automatic eos
 *
 * If @automatic_eos is %TRUE, @src will automatically go EOS if a buffer
 * after the total size is returned. By default this is %TRUE but sources
 * that can't return an authoritative size and only know that they're EOS
 * when trying to read more should set this to %FALSE.
 *
 * Since: 1.4
 */
void
gst_base_src_set_automatic_eos (MyBaseSrc * src, gboolean automatic_eos)
{
  g_return_if_fail (GST_IS_MY_BASE_SRC (src));

  g_atomic_int_set (&src->priv->automatic_eos, automatic_eos);
}

/**
 * gst_base_src_set_async:
 * @src: base source instance
 * @async: new async mode
 *
 * Configure async behaviour in @src, no state change will block. The open,
 * close, start, stop, play and pause virtual methods will be executed in a
 * different thread and are thus allowed to perform blocking operations. Any
 * blocking operation should be unblocked with the unlock vmethod.
 */
void
gst_base_src_set_async (MyBaseSrc * src, gboolean async)
{
  g_return_if_fail (GST_IS_MY_BASE_SRC (src));

  GST_OBJECT_LOCK (src);
  src->priv->async = async;
  GST_OBJECT_UNLOCK (src);
}

/**
 * gst_base_src_is_async:
 * @src: base source instance
 *
 * Get the current async behaviour of @src. See also gst_base_src_set_async().
 *
 * Returns: %TRUE if @src is operating in async mode.
 */
gboolean
gst_base_src_is_async (MyBaseSrc * src)
{
  gboolean res;

  g_return_val_if_fail (GST_IS_MY_BASE_SRC (src), FALSE);

  GST_OBJECT_LOCK (src);
  res = src->priv->async;
  GST_OBJECT_UNLOCK (src);

  return res;
}

/**
 * gst_base_src_set_blocksize:
 * @src: the source
 * @blocksize: the new blocksize in bytes
 *
 * Set the number of bytes that @src will push out with each buffer. When
 * @blocksize is set to -1, a default length will be used.
 */
void
gst_base_src_set_blocksize (MyBaseSrc * src, guint blocksize)
{
  g_return_if_fail (GST_IS_MY_BASE_SRC (src));

  GST_OBJECT_LOCK (src);
  src->blocksize = blocksize;
  GST_OBJECT_UNLOCK (src);
}

/**
 * gst_base_src_get_blocksize:
 * @src: the source
 *
 * Get the number of bytes that @src will push out with each buffer.
 *
 * Returns: the number of bytes pushed with each buffer.
 */
guint
gst_base_src_get_blocksize (MyBaseSrc * src)
{
  gint res;

  g_return_val_if_fail (GST_IS_MY_BASE_SRC (src), 0);

  GST_OBJECT_LOCK (src);
  res = src->blocksize;
  GST_OBJECT_UNLOCK (src);

  return res;
}


/**
 * gst_base_src_set_do_timestamp:
 * @src: the source
 * @timestamp: enable or disable timestamping
 *
 * Configure @src to automatically timestamp outgoing buffers based on the
 * current running_time of the pipeline. This property is mostly useful for live
 * sources.
 */
void
gst_base_src_set_do_timestamp (MyBaseSrc * src, gboolean timestamp)
{
  g_return_if_fail (GST_IS_MY_BASE_SRC (src));

  GST_OBJECT_LOCK (src);
  src->priv->do_timestamp = timestamp;
  if (timestamp && src->segment.format != GST_FORMAT_TIME)
    gst_segment_init (&src->segment, GST_FORMAT_TIME);
  GST_OBJECT_UNLOCK (src);
}

/**
 * gst_base_src_get_do_timestamp:
 * @src: the source
 *
 * Query if @src timestamps outgoing buffers based on the current running_time.
 *
 * Returns: %TRUE if the base class will automatically timestamp outgoing buffers.
 */
gboolean
gst_base_src_get_do_timestamp (MyBaseSrc * src)
{
  gboolean res;

  g_return_val_if_fail (GST_IS_MY_BASE_SRC (src), FALSE);

  GST_OBJECT_LOCK (src);
  res = src->priv->do_timestamp;
  GST_OBJECT_UNLOCK (src);

  return res;
}

/**
 * gst_base_src_new_seamless_segment:
 * @src: The source
 * @start: The new start value for the segment
 * @stop: Stop value for the new segment
 * @time: The new time value for the start of the new segment
 *
 * Prepare a new seamless segment for emission downstream. This function must
 * only be called by derived sub-classes, and only from the create() function,
 * as the stream-lock needs to be held.
 *
 * The format for the new segment will be the current format of the source, as
 * configured with gst_base_src_set_format()
 *
 * Returns: %TRUE if preparation of the seamless segment succeeded.
 */
gboolean
gst_base_src_new_seamless_segment (MyBaseSrc * src, gint64 start, gint64 stop,
    gint64 time)
{
  gboolean res = TRUE;

  GST_OBJECT_LOCK (src);

  src->segment.base = gst_segment_to_running_time (&src->segment,
      src->segment.format, src->segment.position);
  src->segment.position = src->segment.start = start;
  src->segment.stop = stop;
  src->segment.time = time;

  /* Mark pending segment. Will be sent before next data */
  src->priv->segment_pending = TRUE;
  src->priv->segment_seqnum = gst_util_seqnum_next ();

  GST_DEBUG_OBJECT (src,
      "Starting new seamless segment. Start %" GST_TIME_FORMAT " stop %"
      GST_TIME_FORMAT " time %" GST_TIME_FORMAT " base %" GST_TIME_FORMAT,
      GST_TIME_ARGS (start), GST_TIME_ARGS (stop), GST_TIME_ARGS (time),
      GST_TIME_ARGS (src->segment.base));

  GST_OBJECT_UNLOCK (src);

  src->priv->discont = TRUE;
  src->running = TRUE;

  return res;
}

gboolean
gst_base_src_set_caps (MyBaseSrc * src, GstCaps * caps)
{
  MyBaseSrcClass *bclass;
  gboolean res = TRUE;
  GstCaps *current_caps;

  bclass = GST_MY_BASE_SRC_GET_CLASS (src);

  current_caps = gst_pad_get_current_caps (GST_MY_BASE_SRC_PAD (src));
  if (current_caps && gst_caps_is_equal (current_caps, caps)) {
    GST_DEBUG_OBJECT (src, "New caps equal to old ones: %" GST_PTR_FORMAT,
        caps);
    res = TRUE;
  } else {
    if (bclass->set_caps)
      res = bclass->set_caps (src, caps);

    if (res)
      res = gst_pad_push_event (src->srcpad, gst_event_new_caps (caps));
  }

  if (current_caps)
    gst_caps_unref (current_caps);

  return res;
}

static GstCaps *
gst_base_src_default_get_caps (MyBaseSrc * bsrc, GstCaps * filter)
{
  GstCaps *caps = NULL;
  GstPadTemplate *pad_template;
  MyBaseSrcClass *bclass;

  bclass = GST_MY_BASE_SRC_GET_CLASS (bsrc);

  pad_template =
      gst_element_class_get_pad_template (GST_ELEMENT_CLASS (bclass), "src");

  if (pad_template != NULL) {
    caps = gst_pad_template_get_caps (pad_template);

    if (filter) {
      GstCaps *intersection;

      intersection =
          gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
      gst_caps_unref (caps);
      caps = intersection;
    }
  }
  return caps;
}

static GstCaps *
gst_base_src_default_fixate (MyBaseSrc * bsrc, GstCaps * caps)
{
  GST_DEBUG_OBJECT (bsrc, "using default caps fixate function");
  return gst_caps_fixate (caps);
}

static GstCaps *
gst_base_src_fixate (MyBaseSrc * bsrc, GstCaps * caps)
{
  MyBaseSrcClass *bclass;

  bclass = GST_MY_BASE_SRC_GET_CLASS (bsrc);

  if (bclass->fixate)
    caps = bclass->fixate (bsrc, caps);

  return caps;
}

static gboolean
gst_base_src_default_query (MyBaseSrc * src, GstQuery * query)
{
  gboolean res;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_POSITION:
    {
      GstFormat format;

      gst_query_parse_position (query, &format, NULL);

      GST_DEBUG_OBJECT (src, "position query in format %s",
          gst_format_get_name (format));

      switch (format) {
        case GST_FORMAT_PERCENT:
        {
          gint64 percent;
          gint64 position;
          gint64 duration;

          GST_OBJECT_LOCK (src);
          position = src->segment.position;
          duration = src->segment.duration;
          GST_OBJECT_UNLOCK (src);

          if (position != -1 && duration != -1) {
            if (position < duration)
              percent = gst_util_uint64_scale (GST_FORMAT_PERCENT_MAX, position,
                  duration);
            else
              percent = GST_FORMAT_PERCENT_MAX;
          } else
            percent = -1;

          gst_query_set_position (query, GST_FORMAT_PERCENT, percent);
          res = TRUE;
          break;
        }
        default:
        {
          gint64 position;
          GstFormat seg_format;

          GST_OBJECT_LOCK (src);
          position =
              gst_segment_to_stream_time (&src->segment, src->segment.format,
              src->segment.position);
          seg_format = src->segment.format;
          GST_OBJECT_UNLOCK (src);

          if (position != -1) {
            /* convert to requested format */
            res =
                gst_pad_query_convert (src->srcpad, seg_format,
                position, format, &position);
          } else
            res = TRUE;

          if (res)
            gst_query_set_position (query, format, position);

          break;
        }
      }
      break;
    }
    case GST_QUERY_DURATION:
    {
      GstFormat format;

      gst_query_parse_duration (query, &format, NULL);

      GST_DEBUG_OBJECT (src, "duration query in format %s",
          gst_format_get_name (format));

      switch (format) {
        case GST_FORMAT_PERCENT:
          gst_query_set_duration (query, GST_FORMAT_PERCENT,
              GST_FORMAT_PERCENT_MAX);
          res = TRUE;
          break;
        default:
        {
          gint64 duration;
          GstFormat seg_format;
          guint length = 0;

          /* may have to refresh duration */
          gst_base_src_update_length (src, 0, &length,
              g_atomic_int_get (&src->priv->dynamic_size));

          /* this is the duration as configured by the subclass. */
          GST_OBJECT_LOCK (src);
          duration = src->segment.duration;
          seg_format = src->segment.format;
          GST_OBJECT_UNLOCK (src);

          GST_LOG_OBJECT (src, "duration %" G_GINT64_FORMAT ", format %s",
              duration, gst_format_get_name (seg_format));

          if (duration != -1) {
            /* convert to requested format, if this fails, we have a duration
             * but we cannot answer the query, we must return FALSE. */
            res =
                gst_pad_query_convert (src->srcpad, seg_format,
                duration, format, &duration);
          } else {
            /* The subclass did not configure a duration, we assume that the
             * media has an unknown duration then and we return TRUE to report
             * this. Note that this is not the same as returning FALSE, which
             * means that we cannot report the duration at all. */
            res = TRUE;
          }

          if (res)
            gst_query_set_duration (query, format, duration);

          break;
        }
      }
      break;
    }

    case GST_QUERY_SEEKING:
    {
        res = FALSE;
        break;
    }
    case GST_QUERY_SEGMENT:
    {
      GstFormat format;
      gint64 start, stop;

      GST_OBJECT_LOCK (src);

      format = src->segment.format;

      start =
          gst_segment_to_stream_time (&src->segment, format,
          src->segment.start);
      if ((stop = src->segment.stop) == -1)
        stop = src->segment.duration;
      else
        stop = gst_segment_to_stream_time (&src->segment, format, stop);

      gst_query_set_segment (query, src->segment.rate, format, start, stop);

      GST_OBJECT_UNLOCK (src);
      res = TRUE;
      break;
    }

    case GST_QUERY_FORMATS:
    {
      gst_query_set_formats (query, 3, GST_FORMAT_DEFAULT,
          GST_FORMAT_BYTES, GST_FORMAT_PERCENT);
      res = TRUE;
      break;
    }
    case GST_QUERY_CONVERT:
    {
      GstFormat src_fmt, dest_fmt;
      gint64 src_val, dest_val;

      gst_query_parse_convert (query, &src_fmt, &src_val, &dest_fmt, &dest_val);

      /* we can only convert between equal formats... */
      if (src_fmt == dest_fmt) {
        dest_val = src_val;
        res = TRUE;
      } else
        res = FALSE;

      gst_query_set_convert (query, src_fmt, src_val, dest_fmt, dest_val);
      break;
    }
    case GST_QUERY_LATENCY:
    case GST_QUERY_JITTER:
    case GST_QUERY_RATE:
      res = FALSE;
      break;
    case GST_QUERY_BUFFERING:
    {
      GstFormat format, seg_format;
      gint64 start, stop, estimated;

      gst_query_parse_buffering_range (query, &format, NULL, NULL, NULL);

      GST_DEBUG_OBJECT (src, "buffering query in format %s",
          gst_format_get_name (format));

      GST_OBJECT_LOCK (src);
      if (src->random_access) {
        estimated = 0;
        start = 0;
        if (format == GST_FORMAT_PERCENT)
          stop = GST_FORMAT_PERCENT_MAX;
        else
          stop = src->segment.duration;
      } else {
        estimated = -1;
        start = -1;
        stop = -1;
      }
      seg_format = src->segment.format;
      GST_OBJECT_UNLOCK (src);

      /* convert to required format. When the conversion fails, we can't answer
       * the query. When the value is unknown, we can don't perform conversion
       * but report TRUE. */
      if (format != GST_FORMAT_PERCENT && stop != -1) {
        res = gst_pad_query_convert (src->srcpad, seg_format,
            stop, format, &stop);
      } else {
        res = TRUE;
      }
      if (res && format != GST_FORMAT_PERCENT && start != -1)
        res = gst_pad_query_convert (src->srcpad, seg_format,
            start, format, &start);

      gst_query_set_buffering_range (query, format, start, stop, estimated);
      break;
    }
    case GST_QUERY_SCHEDULING:
    {
      gboolean random_access;

      random_access = gst_base_src_is_random_access (src);

      /* we can operate in getrange mode if the native format is bytes
       * and we are seekable, this condition is set in the random_access
       * flag and is set in the _start() method. */
      gst_query_set_scheduling (query, GST_SCHEDULING_FLAG_SEEKABLE, 1, -1, 0);
      if (random_access)
        gst_query_add_scheduling_mode (query, GST_PAD_MODE_PULL);
      gst_query_add_scheduling_mode (query, GST_PAD_MODE_PUSH);

      res = TRUE;
      break;
    }
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
    case GST_QUERY_URI:{
      if (GST_IS_URI_HANDLER (src)) {
        gchar *uri = gst_uri_handler_get_uri (GST_URI_HANDLER (src));

        if (uri != NULL) {
          gst_query_set_uri (query, uri);
          g_free (uri);
          res = TRUE;
        } else {
          res = FALSE;
        }
      } else {
        res = FALSE;
      }
      break;
    }
    default:
      res = FALSE;
      break;
  }
  GST_DEBUG_OBJECT (src, "query %s returns %d", GST_QUERY_TYPE_NAME (query),
      res);

  return res;
}

static gboolean
gst_base_src_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
    MyBaseSrc *src = GST_MY_BASE_SRC (parent);
    MyBaseSrcClass *bclass = GST_MY_BASE_SRC_GET_CLASS (src);
    gboolean result = FALSE;

    if (bclass->query) {
        result = bclass->query (src, query);
    }

    return result;
}

static GstFlowReturn
gst_base_src_default_alloc (MyBaseSrc * src, guint64 offset,
    guint size, GstBuffer ** buffer)
{
  GstFlowReturn ret;
  MyBaseSrcPrivate *priv = src->priv;
  GstBufferPool *pool = NULL;
  GstAllocator *allocator = NULL;
  GstAllocationParams params;

  GST_OBJECT_LOCK (src);
  if (priv->pool) {
    pool = gst_object_ref (priv->pool);
  } else if (priv->allocator) {
    allocator = gst_object_ref (priv->allocator);
  }
  params = priv->params;
  GST_OBJECT_UNLOCK (src);

  if (pool) {
    ret = gst_buffer_pool_acquire_buffer (pool, buffer, NULL);
  } else if (size != -1) {
    *buffer = gst_buffer_new_allocate (allocator, size, &params);
    if (G_UNLIKELY (*buffer == NULL))
      goto alloc_failed;

    ret = GST_FLOW_OK;
  } else {
    GST_WARNING_OBJECT (src, "Not trying to alloc %u bytes. Blocksize not set?",
        size);
    goto alloc_failed;
  }

done:
  if (pool)
    gst_object_unref (pool);
  if (allocator)
    gst_object_unref (allocator);

  return ret;

  /* ERRORS */
alloc_failed:
  {
    GST_ERROR_OBJECT (src, "Failed to allocate %u bytes", size);
    ret = GST_FLOW_ERROR;
    goto done;
  }
}

static GstFlowReturn
gst_base_src_default_create (MyBaseSrc * src, guint64 offset,
    guint size, GstBuffer ** buffer)
{
  MyBaseSrcClass *bclass;
  GstFlowReturn ret;
  GstBuffer *res_buf;

  bclass = GST_MY_BASE_SRC_GET_CLASS (src);

  if (G_UNLIKELY (!bclass->alloc))
    goto no_function;
  if (G_UNLIKELY (!bclass->fill))
    goto no_function;

  if (*buffer == NULL) {
    /* downstream did not provide us with a buffer to fill, allocate one
     * ourselves */
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

/* all events send to this element directly. This is mainly done from the
 * application.
 */
static gboolean
gst_base_src_send_event (GstElement * element, GstEvent * event)
{
  MyBaseSrc *src;
  gboolean result = FALSE;
  MyBaseSrcClass *bclass;

  src = GST_MY_BASE_SRC (element);
  bclass = GST_MY_BASE_SRC_GET_CLASS (src);

  GST_DEBUG_OBJECT (src, "handling event %p %" GST_PTR_FORMAT, event, event);

  switch (GST_EVENT_TYPE (event)) {
      /* bidirectional events */
    case GST_EVENT_FLUSH_START:
      GST_DEBUG_OBJECT (src, "pushing flush-start event downstream");

      result = gst_pad_push_event (src->srcpad, event);
      gst_base_src_set_flushing (src, TRUE);
      event = NULL;
      break;
    case GST_EVENT_FLUSH_STOP:
    {
      gboolean start;

      GST_PAD_STREAM_LOCK (src->srcpad);
      gst_base_src_set_flushing (src, FALSE);

      GST_DEBUG_OBJECT (src, "pushing flush-stop event downstream");
      result = gst_pad_push_event (src->srcpad, event);

      /* For external flush, restart the task .. */
      GST_LIVE_LOCK (src);
      src->priv->segment_pending = TRUE;

      GST_OBJECT_LOCK (src->srcpad);
      start = (GST_PAD_MODE (src->srcpad) == GST_PAD_MODE_PUSH);
      GST_OBJECT_UNLOCK (src->srcpad);

      /* ... and for live sources, only if in playing state */
      if (!src->live_running) {
          start = FALSE;
      }

      if (start)
        gst_pad_start_task (src->srcpad, (GstTaskFunction) gst_base_src_loop,
            src->srcpad, NULL);

      GST_LIVE_UNLOCK (src);
      GST_PAD_STREAM_UNLOCK (src->srcpad);

      event = NULL;
      break;
    }

      /* downstream serialized events */
    case GST_EVENT_EOS:
    {
      gboolean push_mode;

      /* queue EOS and make sure the task or pull function performs the EOS
       * actions.
       *
       * For push mode, This will be done in 3 steps. It is required to not
       * block here as gst_element_send_event() will hold the STATE_LOCK, hence
       * blocking would prevent asynchronous state change to complete.
       *
       * 1. We stop the streaming thread
       * 2. We set the pending eos
       * 3. We start the streaming thread again, so it is performed
       *    asynchronously.
       *
       * For pull mode, we simply mark the pending EOS without flushing.
       */

      GST_OBJECT_LOCK (src->srcpad);
      push_mode = GST_PAD_MODE (src->srcpad) == GST_PAD_MODE_PUSH;
      GST_OBJECT_UNLOCK (src->srcpad);

      if (push_mode) {
        gst_base_src_set_flushing (src, TRUE);

        GST_PAD_STREAM_LOCK (src->srcpad);
        gst_base_src_set_flushing (src, FALSE);

        GST_OBJECT_LOCK (src);
        g_atomic_int_set (&src->priv->has_pending_eos, TRUE);
        if (src->priv->pending_eos)
          gst_event_unref (src->priv->pending_eos);
        src->priv->pending_eos = event;
        GST_OBJECT_UNLOCK (src);

        GST_DEBUG_OBJECT (src,
            "EOS marked, start task for asynchronous handling");
        gst_pad_start_task (src->srcpad, (GstTaskFunction) gst_base_src_loop,
            src->srcpad, NULL);

        GST_PAD_STREAM_UNLOCK (src->srcpad);
      } else {
        /* In pull mode, we need not to return flushing to downstream, though
         * the stream lock is not kept after getrange was unblocked */
        GST_OBJECT_LOCK (src);
        g_atomic_int_set (&src->priv->has_pending_eos, TRUE);
        if (src->priv->pending_eos)
          gst_event_unref (src->priv->pending_eos);
        src->priv->pending_eos = event;
        GST_OBJECT_UNLOCK (src);

        gst_base_src_set_pool_flushing (src, TRUE);
        if (bclass->unlock)
          bclass->unlock (src);

        GST_PAD_STREAM_LOCK (src->srcpad);
        if (bclass->unlock_stop)
          bclass->unlock_stop (src);
        gst_base_src_set_pool_flushing (src, TRUE);
        GST_PAD_STREAM_UNLOCK (src->srcpad);
      }


      event = NULL;
      result = TRUE;
      break;
    }
    case GST_EVENT_SEGMENT:
      /* sending random SEGMENT downstream can break sync. */
      break;
    case GST_EVENT_TAG:
    case GST_EVENT_SINK_MESSAGE:
    case GST_EVENT_CUSTOM_DOWNSTREAM:
    case GST_EVENT_CUSTOM_BOTH:
    case GST_EVENT_PROTECTION:
      /* Insert TAG, CUSTOM_DOWNSTREAM, CUSTOM_BOTH, PROTECTION in the dataflow */
      GST_OBJECT_LOCK (src);
      src->priv->pending_events =
          g_list_append (src->priv->pending_events, event);
      g_atomic_int_set (&src->priv->have_events, TRUE);
      GST_OBJECT_UNLOCK (src);
      event = NULL;
      result = TRUE;
      break;
    case GST_EVENT_BUFFERSIZE:
      /* does not seem to make much sense currently */
      break;

      /* upstream events */
    case GST_EVENT_QOS:
      /* elements should override send_event and do something */
      break;
    case GST_EVENT_SEEK:
    {
      break;
    }
    case GST_EVENT_NAVIGATION:
      /* could make sense for elements that do something with navigation events
       * but then they would need to override the send_event function */
      break;
    case GST_EVENT_LATENCY:
      /* does not seem to make sense currently */
      break;

      /* custom events */
    case GST_EVENT_CUSTOM_UPSTREAM:
      /* override send_event if you want this */
      break;
    case GST_EVENT_CUSTOM_DOWNSTREAM_OOB:
    case GST_EVENT_CUSTOM_BOTH_OOB:
      /* insert a random custom event into the pipeline */
      GST_DEBUG_OBJECT (src, "pushing custom OOB event downstream");
      result = gst_pad_push_event (src->srcpad, event);
      /* we gave away the ref to the event in the push */
      event = NULL;
      break;
    default:
      break;
  }
  /* if we still have a ref to the event, unref it now */
  if (event)
    gst_event_unref (event);

  return result;
}

static void
gst_base_src_update_qos (MyBaseSrc * src, gdouble proportion, GstClockTimeDiff diff, GstClockTime timestamp)
{
    GST_OBJECT_LOCK (src);
    src->priv->proportion = proportion;
    src->priv->earliest_time = timestamp + diff;
    GST_OBJECT_UNLOCK (src);
}


static gboolean
gst_base_src_default_event (MyBaseSrc * src, GstEvent * event)
{
    gboolean result;
    GST_DEBUG_OBJECT (src, "handle event %" GST_PTR_FORMAT, event);

    switch (GST_EVENT_TYPE (event)) {
        case GST_EVENT_SEEK:
            goto not_seekable;
            break;
        case GST_EVENT_FLUSH_START:
            /* cancel any blocking getrange, is normally called
             * when in pull mode. */
            result = gst_base_src_set_flushing (src, TRUE);
            break;
        case GST_EVENT_FLUSH_STOP:
            result = gst_base_src_set_flushing (src, FALSE);
            break;
        case GST_EVENT_QOS:
            {
                gdouble proportion;
                GstClockTimeDiff diff;
                GstClockTime timestamp;

                gst_event_parse_qos (event, NULL, &proportion, &diff, &timestamp);
                gst_base_src_update_qos (src, proportion, diff, timestamp);
                result = TRUE;
                break;
            }
        case GST_EVENT_RECONFIGURE:
            result = TRUE;
            break;
        case GST_EVENT_LATENCY:
            result = TRUE;
            break;
        default:
            result = FALSE;
            break;
    }
    return result;

    /* ERRORS */
not_seekable:
    GST_DEBUG_OBJECT (src, "is not seekable");
    return FALSE;
}

static gboolean
gst_base_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  MyBaseSrc *src;
  MyBaseSrcClass *bclass;
  gboolean result = FALSE;

  src = GST_MY_BASE_SRC (parent);
  bclass = GST_MY_BASE_SRC_GET_CLASS (src);

  if (bclass->event) {
    if (!(result = bclass->event (src, event)))
      goto subclass_failed;
  }

done:
  gst_event_unref (event);

  return result;

  /* ERRORS */
subclass_failed:
  {
    GST_DEBUG_OBJECT (src, "subclass refused event");
    goto done;
  }
}

static void
gst_base_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  MyBaseSrc *src;

  src = GST_MY_BASE_SRC (object);

  switch (prop_id) {
    case PROP_BLOCKSIZE:
      gst_base_src_set_blocksize (src, g_value_get_uint (value));
      break;
    case PROP_NUM_BUFFERS:
      src->num_buffers = g_value_get_int (value);
      break;
#ifndef GST_REMOVE_DEPRECATED
    case PROP_TYPEFIND:
      src->typefind = g_value_get_boolean (value);
      break;
#endif
    case PROP_DO_TIMESTAMP:
      gst_base_src_set_do_timestamp (src, g_value_get_boolean (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_base_src_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  MyBaseSrc *src;

  src = GST_MY_BASE_SRC (object);

  switch (prop_id) {
    case PROP_BLOCKSIZE:
      g_value_set_uint (value, gst_base_src_get_blocksize (src));
      break;
    case PROP_NUM_BUFFERS:
      g_value_set_int (value, src->num_buffers);
      break;
#ifndef GST_REMOVE_DEPRECATED
    case PROP_TYPEFIND:
      g_value_set_boolean (value, src->typefind);
      break;
#endif
    case PROP_DO_TIMESTAMP:
      g_value_set_boolean (value, gst_base_src_get_do_timestamp (src));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_base_src_update_length (MyBaseSrc * src, guint64 offset, guint * length,
    gboolean force)
{
  guint64 size, maxsize;
  MyBaseSrcClass *bclass;
  gint64 stop;

  /* only operate if we are working with bytes */
  if (src->segment.format != GST_FORMAT_BYTES)
    return TRUE;

  bclass = GST_MY_BASE_SRC_GET_CLASS (src);

  stop = src->segment.stop;
  /* get total file size */
  size = src->segment.duration;

  /* when not doing automatic EOS, just use the stop position. We don't use
   * the size to check for EOS */
  if (!g_atomic_int_get (&src->priv->automatic_eos))
    maxsize = stop;
  /* Otherwise, the max amount of bytes to read is the total
   * size or up to the segment.stop if present. */
  else if (stop != -1)
    maxsize = size != -1 ? MIN (size, stop) : stop;
  else
    maxsize = size;

  GST_DEBUG_OBJECT (src,
      "reading offset %" G_GUINT64_FORMAT ", length %u, size %" G_GINT64_FORMAT
      ", segment.stop %" G_GINT64_FORMAT ", maxsize %" G_GINT64_FORMAT, offset,
      *length, size, stop, maxsize);

  /* check size if we have one */
  if (maxsize != -1) {
    /* if we run past the end, check if the file became bigger and
     * retry.  Mind wrap when checking. */
    if (G_UNLIKELY (offset >= maxsize || offset + *length >= maxsize || force)) {
      /* see if length of the file changed */
      if (bclass->get_size)
        if (!bclass->get_size (src, &size))
          size = -1;

      /* when not doing automatic EOS, just use the stop position. We don't use
       * the size to check for EOS */
      if (!g_atomic_int_get (&src->priv->automatic_eos))
        maxsize = stop;
      /* Otherwise, the max amount of bytes to read is the total
       * size or up to the segment.stop if present. */
      else if (stop != -1)
        maxsize = size != -1 ? MIN (size, stop) : stop;
      else
        maxsize = size;

      if (maxsize != -1) {
        /* if we are at or past the end, EOS */
        if (G_UNLIKELY (offset >= maxsize))
          goto unexpected_length;

        /* else we can clip to the end */
        if (G_UNLIKELY (offset + *length >= maxsize))
          *length = maxsize - offset;
      }
    }
  }

  /* keep track of current duration. segment is in bytes, we checked
   * that above. */
  GST_OBJECT_LOCK (src);
  src->segment.duration = size;
  GST_OBJECT_UNLOCK (src);

  return TRUE;

  /* ERRORS */
unexpected_length:
  {
    GST_WARNING_OBJECT (src, "processing at or past EOS");
    return FALSE;
  }
}

static GstFlowReturn gst_base_src_get_range2 (GstPad * pad, GstBuffer **buf)
{
    MyBaseSrc *src;
    MyBaseSrcClass *bclass;
    GstFlowReturn ret;
    src = GST_MY_BASE_SRC (GST_OBJECT_PARENT (pad));
    bclass = GST_MY_BASE_SRC_GET_CLASS (src);
    ret = bclass->create2 (pad, buf);
    return ret;
}

static gboolean
gst_base_src_is_random_access (MyBaseSrc * src)
{
  /* we need to start the basesrc to check random access */
  if (!GST_MY_BASE_SRC_IS_STARTED (src)) {
    GST_LOG_OBJECT (src, "doing start/stop to check get_range support");
    if (gst_base_src_start_wait (src) != GST_FLOW_OK)
        goto start_failed;
  }


  return src->random_access;

  /* ERRORS */
start_failed:
  {
    GST_DEBUG_OBJECT (src, "failed to start");
    return FALSE;
  }
}

/* Called with STREAM_LOCK */
static void
gst_base_src_loop (GstPad * pad)
{
    GstFlowReturn ret;
    MyBaseSrc *src = GST_MY_BASE_SRC (GST_OBJECT_PARENT (pad));
    GstBuffer *buf = NULL;
    gint64 position = -1;
    gboolean eos = FALSE;
    GList *pending_events = NULL, *tmp;


    /* Just leave immediately if we're flushing */
    GST_LIVE_LOCK (src);
    if (G_UNLIKELY (src->priv->flushing || GST_PAD_IS_FLUSHING (pad)))
        goto flushing;
    GST_LIVE_UNLOCK (src);

    /* Just return if EOS is pushed again, as the app might be unaware that an
     * EOS have been sent already */
    if (GST_PAD_IS_EOS (pad)) {
        GST_DEBUG_OBJECT (src, "Pad is marked as EOS, pause the task");
        gst_pad_pause_task (pad);
        goto done;
    }

    /* The stream-start event could've caused something to flush us */
    GST_LIVE_LOCK (src);
    if (G_UNLIKELY (src->priv->flushing || GST_PAD_IS_FLUSHING (pad)))
        goto flushing;
    GST_LIVE_UNLOCK (src);

    /* check if we need to renegotiate */
    if (gst_pad_check_reconfigure (pad)) {
        if (!gst_base_src_negotiate (pad)) {
            gst_pad_mark_reconfigure (pad);
            if (GST_PAD_IS_FLUSHING (pad)) {
                GST_LIVE_LOCK (src);
                goto flushing;
            } else {
                goto negotiate_failed;
            }
        }
    }

    GST_LIVE_LOCK (src);

    if (G_UNLIKELY (src->priv->flushing || GST_PAD_IS_FLUSHING (pad)))
        goto flushing;

    /* clean up just in case we got interrupted or so last time round */
    if (src->priv->pending_bufferlist != NULL) {
        gst_buffer_list_unref (src->priv->pending_bufferlist);
        src->priv->pending_bufferlist = NULL;
    }

    ret = gst_base_src_get_range2 (pad, &buf);
    if (G_UNLIKELY (ret != GST_FLOW_OK)) {
        GST_INFO_OBJECT (src, "pausing after gst_base_src_get_range() = %s",
                gst_flow_get_name (ret));
        GST_LIVE_UNLOCK (src);
        goto pause;
    }

    /* Note: at this point buf might be a single buf returned which we own or
     * the first buf of a pending buffer list submitted via submit_buffer_list(),
     * in which case the buffer is owned by the pending buffer list and not us. */
    g_assert (buf != NULL);

    /* push events to close/start our segment before we push the buffer. */
    if (G_UNLIKELY (src->priv->segment_pending)) {
        GstEvent *seg_event = gst_event_new_segment (&src->segment);

        gst_event_set_seqnum (seg_event, src->priv->segment_seqnum);
        src->priv->segment_seqnum = gst_util_seqnum_next ();
        gst_pad_push_event (pad, seg_event);
        src->priv->segment_pending = FALSE;
    }

    if (g_atomic_int_get (&src->priv->have_events)) {
        GST_OBJECT_LOCK (src);
        /* take the events */
        pending_events = src->priv->pending_events;
        src->priv->pending_events = NULL;
        g_atomic_int_set (&src->priv->have_events, FALSE);
        GST_OBJECT_UNLOCK (src);
    }

    /* Push out pending events if any */
    if (G_UNLIKELY (pending_events != NULL)) {
        for (tmp = pending_events; tmp; tmp = g_list_next (tmp)) {
            GstEvent *ev = (GstEvent *) tmp->data;
            gst_pad_push_event (pad, ev);
        }
        g_list_free (pending_events);
    }

    /* figure out the new position */
    switch (src->segment.format) {
        case GST_FORMAT_BYTES: // we don't support bytes now
            break;
        case GST_FORMAT_TIME:
            {

                GstClockTime start, duration;
                start = GST_BUFFER_TIMESTAMP (buf);
                duration = GST_BUFFER_DURATION (buf);

                if (GST_CLOCK_TIME_IS_VALID (start))
                    position = start;
                else
                    position = src->segment.position;

                if (GST_CLOCK_TIME_IS_VALID (duration)) {
                    if (src->segment.rate >= 0.0)
                        position += duration;
                    else if (position > duration)
                        position -= duration;
                    else
                        position = 0;
                }
                break;
            }
        case GST_FORMAT_DEFAULT:
            if (src->segment.rate >= 0.0)
                position = GST_BUFFER_OFFSET_END (buf);
            else
                position = GST_BUFFER_OFFSET (buf);
            break;
        default:
            position = -1;
            break;
    }

    if (position != -1) {
        if (src->segment.rate >= 0.0) {
            /* positive rate, check if we reached the stop */
            if (src->segment.stop != -1) {
                if (position >= src->segment.stop) {
                    eos = TRUE;
                    position = src->segment.stop;
                }
            }
        } else {
            /* negative rate, check if we reached the start. start is always set to
             * something different from -1 */
            if (position <= src->segment.start) {
                eos = TRUE;
                position = src->segment.start;
            }
            /* when going reverse, all buffers are DISCONT */
            src->priv->discont = TRUE;
        }
        GST_OBJECT_LOCK (src);
        src->segment.position = position;
        GST_OBJECT_UNLOCK (src);
    }

    if (G_UNLIKELY (src->priv->discont)) {
        GST_INFO_OBJECT (src, "marking pending DISCONT");
        buf = gst_buffer_make_writable (buf);
        GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DISCONT);
        src->priv->discont = FALSE;
    }
    GST_LIVE_UNLOCK (src);

    /* push buffer or buffer list */
    if (src->priv->pending_bufferlist != NULL) {
        ret = gst_pad_push_list (pad, src->priv->pending_bufferlist);
        src->priv->pending_bufferlist = NULL;
    } else {
        ret = gst_pad_push (pad, buf);
    }

    if (G_UNLIKELY (ret != GST_FLOW_OK)) {
        if (ret == GST_FLOW_NOT_NEGOTIATED) {
            goto not_negotiated;
        }
        GST_INFO_OBJECT (src, "pausing after gst_pad_push() = %s",
                gst_flow_get_name (ret));
        goto pause;
    }

    /* Segment pending means that a new segment was configured
     * during this loop run */
    if (G_UNLIKELY (eos && !src->priv->segment_pending)) {
        GST_INFO_OBJECT (src, "pausing after end of segment");
        ret = GST_FLOW_EOS;
        goto pause;
    }

done:
    return;

    /* special cases */
not_negotiated:
    {
        if (gst_pad_needs_reconfigure (pad)) {
            GST_DEBUG_OBJECT (src, "Retrying to renegotiate");
            return;
        }
        /* fallthrough when push returns NOT_NEGOTIATED and we don't have
         * a pending negotiation request on our srcpad */
    }
negotiate_failed:
    {
        GST_DEBUG_OBJECT (src, "Not negotiated");
        ret = GST_FLOW_NOT_NEGOTIATED;
        goto pause;
    }
flushing:
    {
        GST_DEBUG_OBJECT (src, "we are flushing");
        GST_LIVE_UNLOCK (src);
        ret = GST_FLOW_FLUSHING;
        goto pause;
    }
pause:
    {
        const gchar *reason = gst_flow_get_name (ret);
        GstEvent *event;

        GST_DEBUG_OBJECT (src, "pausing task, reason %s", reason);
        src->running = FALSE;
        gst_pad_pause_task (pad);
        if (ret == GST_FLOW_EOS) {
            gboolean flag_segment;
            GstFormat format;
            gint64 position;

            flag_segment = (src->segment.flags & GST_SEGMENT_FLAG_SEGMENT) != 0;
            format = src->segment.format;
            position = src->segment.position;

            /* perform EOS logic */
            if (src->priv->forced_eos) {
                g_assert (g_atomic_int_get (&src->priv->has_pending_eos));
                GST_OBJECT_LOCK (src);
                event = src->priv->pending_eos;
                src->priv->pending_eos = NULL;
                GST_OBJECT_UNLOCK (src);

            } else if (flag_segment) {
                GstMessage *message;

                message = gst_message_new_segment_done (GST_OBJECT_CAST (src),
                        format, position);
                gst_message_set_seqnum (message, src->priv->seqnum);
                gst_element_post_message (GST_ELEMENT_CAST (src), message);
                event = gst_event_new_segment_done (format, position);
                gst_event_set_seqnum (event, src->priv->seqnum);

            } else {
                event = gst_event_new_eos ();
                gst_event_set_seqnum (event, src->priv->seqnum);
            }

            gst_pad_push_event (pad, event);
            src->priv->forced_eos = FALSE;

        } else if (ret == GST_FLOW_NOT_LINKED || ret <= GST_FLOW_EOS) {
            event = gst_event_new_eos ();
            gst_event_set_seqnum (event, src->priv->seqnum);
            /* for fatal errors we post an error message, post the error
             * first so the app knows about the error first.
             * Also don't do this for FLUSHING because it happens
             * due to flushing and posting an error message because of
             * that is the wrong thing to do, e.g. when we're doing
             * a flushing seek. */
            GST_ELEMENT_FLOW_ERROR (src, ret);
            gst_pad_push_event (pad, event);
        }
        goto done;
    }
}

static gboolean
gst_base_src_set_allocation (MyBaseSrc * basesrc, GstBufferPool * pool,
    GstAllocator * allocator, GstAllocationParams * params)
{
  GstAllocator *oldalloc;
  GstBufferPool *oldpool;
  MyBaseSrcPrivate *priv = basesrc->priv;

  if (pool) {
    GST_DEBUG_OBJECT (basesrc, "activate pool");
    if (!gst_buffer_pool_set_active (pool, TRUE))
      goto activate_failed;
  }

  GST_OBJECT_LOCK (basesrc);
  oldpool = priv->pool;
  priv->pool = pool;

  oldalloc = priv->allocator;
  priv->allocator = allocator;

  if (priv->pool)
    gst_object_ref (priv->pool);
  if (priv->allocator)
    gst_object_ref (priv->allocator);

  if (params)
    priv->params = *params;
  else
    gst_allocation_params_init (&priv->params);
  GST_OBJECT_UNLOCK (basesrc);

  if (oldpool) {
    /* only deactivate if the pool is not the one we're using */
    if (oldpool != pool) {
      GST_DEBUG_OBJECT (basesrc, "deactivate old pool");
      gst_buffer_pool_set_active (oldpool, FALSE);
    }
    gst_object_unref (oldpool);
  }
  if (oldalloc) {
    gst_object_unref (oldalloc);
  }
  return TRUE;

  /* ERRORS */
activate_failed:
  {
    GST_ERROR_OBJECT (basesrc, "failed to activate bufferpool.");
    return FALSE;
  }
}

static void
gst_base_src_set_pool_flushing (MyBaseSrc * basesrc, gboolean flushing)
{
  MyBaseSrcPrivate *priv = basesrc->priv;
  GstBufferPool *pool;

  GST_OBJECT_LOCK (basesrc);
  if ((pool = priv->pool))
    pool = gst_object_ref (pool);
  GST_OBJECT_UNLOCK (basesrc);

  if (pool) {
    gst_buffer_pool_set_flushing (pool, flushing);
    gst_object_unref (pool);
  }
}


static gboolean
gst_base_src_decide_allocation_default (MyBaseSrc * basesrc, GstQuery * query)
{
  GstCaps *outcaps;
  GstBufferPool *pool;
  guint size, min, max;
  GstAllocator *allocator;
  GstAllocationParams params;
  GstStructure *config;
  gboolean update_allocator;

  gst_query_parse_allocation (query, &outcaps, NULL);

  /* we got configuration from our peer or the decide_allocation method,
   * parse them */
  if (gst_query_get_n_allocation_params (query) > 0) {
    /* try the allocator */
    gst_query_parse_nth_allocation_param (query, 0, &allocator, &params);
    update_allocator = TRUE;
  } else {
    allocator = NULL;
    gst_allocation_params_init (&params);
    update_allocator = FALSE;
  }

  if (gst_query_get_n_allocation_pools (query) > 0) {
    gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);

    if (pool == NULL) {
      /* no pool, we can make our own */
      GST_DEBUG_OBJECT (basesrc, "no pool, making new pool");
      pool = gst_buffer_pool_new ();
    }
  } else {
    pool = NULL;
    size = min = max = 0;
  }

  /* now configure */
  if (pool) {
    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_set_params (config, outcaps, size, min, max);
    gst_buffer_pool_config_set_allocator (config, allocator, &params);

    /* buffer pool may have to do some changes */
    if (!gst_buffer_pool_set_config (pool, config)) {
      config = gst_buffer_pool_get_config (pool);

      /* If change are not acceptable, fallback to generic pool */
      if (!gst_buffer_pool_config_validate_params (config, outcaps, size, min,
              max)) {
        GST_DEBUG_OBJECT (basesrc, "unsupported pool, making new pool");

        gst_object_unref (pool);
        pool = gst_buffer_pool_new ();
        gst_buffer_pool_config_set_params (config, outcaps, size, min, max);
        gst_buffer_pool_config_set_allocator (config, allocator, &params);
      }

      if (!gst_buffer_pool_set_config (pool, config))
        goto config_failed;
    }
  }

  if (update_allocator)
    gst_query_set_nth_allocation_param (query, 0, allocator, &params);
  else
    gst_query_add_allocation_param (query, allocator, &params);
  if (allocator)
    gst_object_unref (allocator);

  if (pool) {
    gst_query_set_nth_allocation_pool (query, 0, pool, size, min, max);
    gst_object_unref (pool);
  }

  return TRUE;

config_failed:
  GST_ELEMENT_ERROR (basesrc, RESOURCE, SETTINGS,
      ("Failed to configure the buffer pool"),
      ("Configuration is most likely invalid, please report this issue."));
  gst_object_unref (pool);
  return FALSE;
}

static gboolean
gst_base_src_prepare_allocation (GstPad *pad, GstCaps *caps)
{
    MyBaseSrc *basesrc;
    MyBaseSrcClass *bclass;
    gboolean result = TRUE;
    GstQuery *query;
    GstBufferPool *pool = NULL;
    GstAllocator *allocator = NULL;
    GstAllocationParams params;

    basesrc = GST_MY_BASE_SRC (GST_OBJECT_PARENT (pad));
    bclass = GST_MY_BASE_SRC_GET_CLASS (basesrc);

    /* make query and let peer pad answer, we don't really care if it worked or
     * not, if it failed, the allocation query would contain defaults and the
     * subclass would then set better values if needed */
    query = gst_query_new_allocation (caps, TRUE);
    if (!gst_pad_peer_query (pad, query)) {
        /* not a problem, just debug a little */
        GST_DEBUG_OBJECT (basesrc, "peer ALLOCATION query failed");
    }

    g_assert (bclass->decide_allocation != NULL);
    result = bclass->decide_allocation (basesrc, query);

    GST_DEBUG_OBJECT (basesrc, "ALLOCATION (%d) params: %" GST_PTR_FORMAT, result, query);

    if (!result)
        goto no_decide_allocation;

    /* we got configuration from our peer or the decide_allocation method,
     * parse them */
    if (gst_query_get_n_allocation_params (query) > 0) {
        gst_query_parse_nth_allocation_param (query, 0, &allocator, &params);
    } else {
        allocator = NULL;
        gst_allocation_params_init (&params);
    }

    if (gst_query_get_n_allocation_pools (query) > 0)
        gst_query_parse_nth_allocation_pool (query, 0, &pool, NULL, NULL, NULL);

    result = gst_base_src_set_allocation (basesrc, pool, allocator, &params);

    if (allocator)
        gst_object_unref (allocator);
    if (pool)
        gst_object_unref (pool);

    gst_query_unref (query);

    return result;

    /* Errors */
no_decide_allocation:
    {
        GST_WARNING_OBJECT (basesrc, "Subclass failed to decide allocation");
        gst_query_unref (query);

        return result;
    }
}

/* default negotiation code.
 *
 * Take intersection between src and sink pads, take first
 * caps and fixate.
 */
static gboolean
gst_base_src_default_negotiate (MyBaseSrc * basesrc)
{
  GstCaps *thiscaps;
  GstCaps *caps = NULL;
  GstCaps *peercaps = NULL;
  gboolean result = FALSE;

  /* first see what is possible on our source pad */
  thiscaps = gst_pad_query_caps (GST_MY_BASE_SRC_PAD (basesrc), NULL);
  GST_DEBUG_OBJECT (basesrc, "caps of src: %" GST_PTR_FORMAT, thiscaps);
  /* nothing or anything is allowed, we're done */
  if (thiscaps == NULL || gst_caps_is_any (thiscaps))
    goto no_nego_needed;

  if (G_UNLIKELY (gst_caps_is_empty (thiscaps)))
    goto no_caps;

  /* get the peer caps */
  peercaps = gst_pad_peer_query_caps (GST_MY_BASE_SRC_PAD (basesrc), thiscaps);
  GST_DEBUG_OBJECT (basesrc, "caps of peer: %" GST_PTR_FORMAT, peercaps);
  if (peercaps) {
    /* The result is already a subset of our caps */
    caps = peercaps;
    gst_caps_unref (thiscaps);
  } else {
    /* no peer, work with our own caps then */
    caps = thiscaps;
  }
  if (caps && !gst_caps_is_empty (caps)) {
    /* now fixate */
    GST_DEBUG_OBJECT (basesrc, "have caps: %" GST_PTR_FORMAT, caps);
    if (gst_caps_is_any (caps)) {
      GST_DEBUG_OBJECT (basesrc, "any caps, we stop");
      /* hmm, still anything, so element can do anything and
       * nego is not needed */
      result = TRUE;
    } else {
      caps = gst_base_src_fixate (basesrc, caps);
      GST_DEBUG_OBJECT (basesrc, "fixated to: %" GST_PTR_FORMAT, caps);
      if (gst_caps_is_fixed (caps)) {
        /* yay, fixed caps, use those then, it's possible that the subclass does
         * not accept this caps after all and we have to fail. */
        result = gst_base_src_set_caps (basesrc, caps);
      }
    }
    gst_caps_unref (caps);
  } else {
    if (caps)
      gst_caps_unref (caps);
    GST_DEBUG_OBJECT (basesrc, "no common caps");
  }
  return result;

no_nego_needed:
  {
    GST_DEBUG_OBJECT (basesrc, "no negotiation needed");
    if (thiscaps)
      gst_caps_unref (thiscaps);
    return TRUE;
  }
no_caps:
  {
    GST_ELEMENT_ERROR (basesrc, STREAM, FORMAT,
        ("No supported formats found"),
        ("This element did not produce valid caps"));
    if (thiscaps)
      gst_caps_unref (thiscaps);
    return TRUE;
  }
}

static gboolean
gst_base_src_negotiate (GstPad *pad)
{
    MyBaseSrc *basesrc;
    MyBaseSrcClass *bclass;
    gboolean result;

    basesrc = GST_MY_BASE_SRC (GST_OBJECT_PARENT (pad));
    bclass = GST_MY_BASE_SRC_GET_CLASS (basesrc);

    if (G_LIKELY (bclass->negotiate))
        result = bclass->negotiate (basesrc);
    else
        result = TRUE;

    if (G_LIKELY (result)) {
        GstCaps *caps = gst_pad_get_current_caps (pad);
        result = gst_base_src_prepare_allocation (pad, caps);
        if (caps) {
            gst_caps_unref (caps);
        }
    }
    return result;
}

/**
 * gst_base_src_start_wait:
 * @basesrc: base source instance
 *
 * Wait until the start operation completes.
 *
 * Returns: a #GstFlowReturn.
 */
GstFlowReturn
gst_base_src_start_wait (MyBaseSrc * basesrc)
{
  GstFlowReturn result;

  GST_OBJECT_LOCK (basesrc);
  while (GST_MY_BASE_SRC_IS_STARTING (basesrc)) {
    GST_ASYNC_WAIT (basesrc);
  }
  result = basesrc->priv->start_result;
  GST_OBJECT_UNLOCK (basesrc);

  GST_DEBUG_OBJECT (basesrc, "got %s", gst_flow_get_name (result));

  return result;
}

static gboolean
gst_base_src_set_flushing (MyBaseSrc * basesrc, gboolean flushing)
{
  MyBaseSrcClass *bclass;

  bclass = GST_MY_BASE_SRC_GET_CLASS (basesrc);

  GST_DEBUG_OBJECT (basesrc, "flushing %d", flushing);

  if (flushing) {
    gst_base_src_set_pool_flushing (basesrc, TRUE);
    /* unlock any subclasses to allow turning off the streaming thread */
    if (bclass->unlock)
      bclass->unlock (basesrc);
  }

  /* the live lock is released when we are blocked, waiting for playing,
   * when we sync to the clock or creating a buffer */
  GST_LIVE_LOCK (basesrc);
  basesrc->priv->flushing = flushing;
  if (flushing) {
    /* clear pending EOS if any */
    if (g_atomic_int_get (&basesrc->priv->has_pending_eos)) {
      GST_OBJECT_LOCK (basesrc);
      CLEAR_PENDING_EOS (basesrc);
      basesrc->priv->forced_eos = FALSE;
      GST_OBJECT_UNLOCK (basesrc);
    }

    /* unblock clock sync (if any) or any other blocking thing */
    if (basesrc->clock_id)
      gst_clock_id_unschedule (basesrc->clock_id);
  } else {
    gst_base_src_set_pool_flushing (basesrc, FALSE);

    /* Drop all delayed events */
    GST_OBJECT_LOCK (basesrc);
    if (basesrc->priv->pending_events) {
      g_list_foreach (basesrc->priv->pending_events, (GFunc) gst_event_unref,
          NULL);
      g_list_free (basesrc->priv->pending_events);
      basesrc->priv->pending_events = NULL;
      g_atomic_int_set (&basesrc->priv->have_events, FALSE);
    }
    GST_OBJECT_UNLOCK (basesrc);
  }

  GST_LIVE_SIGNAL (basesrc);
  GST_LIVE_UNLOCK (basesrc);

  if (!flushing) {
    /* Now wait for the stream lock to be released and clear our unlock request */
    //GST_PAD_STREAM_LOCK (basesrc->srcpad);
    if (bclass->unlock_stop)
      bclass->unlock_stop (basesrc);
    //GST_PAD_STREAM_UNLOCK (basesrc->srcpad);
  }

  return TRUE;
}


