#ifndef __GST_TFI_SDI_SRC_H__
#define __GST_TFI_SDI_SRC_H__

#include <gst/gst.h>
#include <gst/base/gstbasesrc.h>

G_BEGIN_DECLS

#define GST_TYPE_TFI_SDI_SRC (tfi_sdi_src_get_type())

typedef struct _TfiSdiSrc TfiSdiSrc;
typedef struct _TfiSdiSrcClass TfiSdiSrcClass;

struct _TfiSdiSrc {
  GstBaseSrc element;
};

struct _TfiSdiSrcClass {
  GstBaseSrcClass parent_class;
};

GType tfi_sdi_src_get_type (void);

G_END_DECLS

#endif /* __GST_TFI_SDI_SRC_H__ */
