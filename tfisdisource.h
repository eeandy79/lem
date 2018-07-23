#ifndef __GST_TFI_SDI_SRC_H__
#define __GST_TFI_SDI_SRC_H__

#include <gst/gst.h>
#include "linux/DeckLinkAPI.h"

G_BEGIN_DECLS

#define GST_TYPE_TFI_SDI_SRC           (tfi_sdi_src_get_type())
#define TFI_SDI_SRC(obj)               (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_TFI_SDI_SRC, TfiSdiSrc))
#define TFI_SDI_SRC_GET_CLASS(obj)     (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_TFI_SDI_SRC, TfiSdiSrcClass))

typedef struct _TfiSdiSrc TfiSdiSrc;
typedef struct _TfiSdiSrcClass TfiSdiSrcClass;

struct _TfiSdiSrc {
    GstElement element;
    GThread *datathread;
    gboolean running;
    gint pad_counter;
    //GQueue pad_queue;

    GstPad *video_pad;
    GstPad *audio_pad;

    IDeckLinkInput *deckLinkInput = NULL;
	IDeckLinkDisplayMode *displayMode = NULL;
    //IDeckLinkInputCallback *cb = NULL;

    GstClock *clock;
    GstClockTime clock_start_time, clock_last_time, clock_epoch;
    GstClockTimeDiff clock_offset;

    /* Everything below protected by mutex */
    GMutex lock;
    GCond cond;
};

struct _TfiSdiSrcClass {
    GstElementClass parent_class;
};

GType tfi_sdi_src_get_type (void);

G_END_DECLS

#endif /* __GST_TFI_SDI_SRC_H__ */
