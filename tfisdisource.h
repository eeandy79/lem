#ifndef __GST_TFI_SDI_SRC_H__
#define __GST_TFI_SDI_SRC_H__

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_TFI_SDI_SRC (tfi_sdi_src_get_type())
#define TFI_SDI_SRC(obj)     (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_TFI_SDI_SRC,TfiSdiSrc))

typedef struct _TfiSdiSrc TfiSdiSrc;
typedef struct _TfiSdiSrcClass TfiSdiSrcClass;

struct _TfiSdiSrc {
  GstElement element;
  GstPad *srcpad;
  gboolean stream_start_pending;
};

struct _TfiSdiSrcClass {
  GstElementClass parent_class;
};

GType tfi_sdi_src_get_type (void);

G_END_DECLS

#endif /* __GST_TFI_SDI_SRC_H__ */
