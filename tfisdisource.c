#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "tfisdisource.h"
#include "gstmyfilter.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <gst/video/video.h>

#include "decklinkconfig.h"

#define LOG_HDL stdout
//#define LOG_HDL fout

static GstClock *testclock;
static GstClockTime basetime;
static FILE* fout;

static GstElementClass *parent_class = NULL;
GST_DEBUG_CATEGORY_STATIC (tfi_sdi_src_debug);
#define GST_CAT_DEFAULT tfi_sdi_src_debug
#define DEFAULT_IS_LIVE TRUE

#define _do_init \
    GST_DEBUG_CATEGORY_INIT (tfi_sdi_src_debug, "tfi_sdi_src", 0, "TFI SDI source plugin");
//G_DEFINE_TYPE_WITH_CODE (TfiSdiSrc, tfi_sdi_src, GST_TYPE_MY_BASE_SRC, _do_init);
G_DEFINE_TYPE_WITH_CODE (TfiSdiSrc, tfi_sdi_src, GST_TYPE_ELEMENT, _do_init);

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src%d",
    GST_PAD_SRC,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate video_template = GST_STATIC_PAD_TEMPLATE ("video",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS 
    ("video/x-raw,format=UYVY,width=1920,height=1080,framerate=25/1"));

static GstStaticPadTemplate audio_template = GST_STATIC_PAD_TEMPLATE ("audio",
        GST_PAD_SRC,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS
        //("audio/x-raw, format={S16LE,S32LE}, channels=2, rate=48000, layout=interleaved"));
        ("audio/x-raw, format={S16LE}, channels=2, rate=48000, layout=interleaved"));


static GstFlowReturn tfi_sdi_src_alloc (TfiSdiSrc *src, guint64 offset, guint size, GstBuffer ** buffer);
static GstFlowReturn create2 (GstPad *pad, GstBuffer ** buffer);
static void* work (void *p);
static void* workpad (void *p);
static bool start (TfiSdiSrc *src);
static void stop (TfiSdiSrc *src);
static GstPad *request_new_pad (GstElement *element, GstPadTemplate *tp, const gchar *name, const GstCaps *caps);
static GstStateChangeReturn gst_base_src_change_state (GstElement *element, GstStateChange transition);
static GstClock *gst_tfi_sdi_src_provide_clock (GstElement * element);
static GstClockTime tfi_decklink_clock_get_internal_time (GstClock * clock);

static gboolean gst_base_src_query (GstPad * pad, GstObject * parent, GstQuery * query);
static gboolean gst_stream_event (GstPad * pad, GstObject * parent, GstEvent * event);
#define GST_TYPE_TFI_DECKLINK_CLOCK \
      (tfi_decklink_clock_get_type())
#define GST_DECKLINK_CLOCK(obj) \
      (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_TFI_DECKLINK_CLOCK,TfiDecklinkClock))
#define GST_DECKLINK_CLOCK_CLASS(klass) \
      (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_TFI_DECKLINK_CLOCK,TfiDecklinkClockClass))
#define GST_IS_Decklink_CLOCK(obj) \
      (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_TFI_DECKLINK_CLOCK))
#define GST_IS_Decklink_CLOCK_CLASS(klass) \
      (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_TFI_DECKLINK_CLOCK))
#define GST_DECKLINK_CLOCK_CAST(obj) \
      ((TfiDecklinkClock*)(obj))


enum
{
    PROP_0,
    PROP_BITRATE
};

static void
tfidecklinksrc_set_property(GObject * object, guint prop_id,
        const GValue * value, GParamSpec * pspec)
{
    TfiSdiSrc *self = (TfiSdiSrc*)(object);
    switch (prop_id) {
        case PROP_BITRATE:
            self->bitrate = g_value_get_int (value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
    return;
}

static void
tfidecklinksrc_get_property (GObject * object, guint prop_id,
        GValue * value, GParamSpec * pspec)
{
    TfiSdiSrc *self = (TfiSdiSrc*)(object);
    switch (prop_id) {
        case PROP_BITRATE:
            g_value_set_int (value, self->bitrate);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
    return;
}

typedef struct _TfiDecklinkClock TfiDecklinkClock;
typedef struct _TfiDecklinkClockClass TfiDecklinkClockClass;

struct _TfiDecklinkClock
{
    GstSystemClock clock;
    TfiSdiSrc *input;
};

struct _TfiDecklinkClockClass
{
    GstSystemClockClass parent_class;
};

GType tfi_decklink_clock_get_type (void);
static GstClock *tfi_decklink_clock_new (const gchar * name);



/*
static GstPad *
request_new_pad (GstElement *element, GstPadTemplate *tp, const gchar *name, const GstCaps *caps)
{
    gchar *pad_name;
    GstPad *pad;
    TfiSdiSrc *src = (TfiSdiSrc*)element;
    
    pad_name = g_strdup_printf ("src%d", src->pad_counter++);
    pad = gst_pad_new_from_template (tp, pad_name);
    g_free(pad_name);

    GstCaps *caps2 = gst_caps_new_simple ("video/x-raw",
            "format", G_TYPE_STRING, "UYVY",
            "width", G_TYPE_INT, 1920,
            "height", G_TYPE_INT, 1080,
            NULL);

    if (gst_pad_set_caps(pad, caps2) == FALSE) {
        printf("xxFail to set caps\n");
        exit(0);
    }



    gst_element_add_pad (GST_ELEMENT (src), pad);
    g_queue_push_tail(&src->pad_queue, pad);

    return pad;
}
*/


static void
tfi_sdi_src_class_init (TfiSdiSrcClass * klass)
{
    GObjectClass *gobject_class;
    GstElementClass *gstelement_class;

    gobject_class = (GObjectClass *) klass;
    gstelement_class = (GstElementClass *) klass;
    parent_class = (GstElementClass*)g_type_class_peek_parent (klass);

    //gstelement_class->request_new_pad = request_new_pad;
    gstelement_class->change_state = gst_base_src_change_state;
    gstelement_class->provide_clock = gst_tfi_sdi_src_provide_clock;

    gobject_class->set_property = tfidecklinksrc_set_property;
    gobject_class->get_property = tfidecklinksrc_get_property;

    gst_element_class_add_pad_template (gstelement_class, gst_static_pad_template_get (&src_template));
    gst_element_class_add_pad_template (gstelement_class, gst_static_pad_template_get (&video_template));
    gst_element_class_add_pad_template (gstelement_class, gst_static_pad_template_get (&audio_template));
    gst_element_class_set_static_metadata (gstelement_class,
            "TFI SDI source", "Source/Video",
            "Integrate with Blackmagic SDI card", "Andy Chang <andy.chang@tfidm.com>");

    g_object_class_install_property (gobject_class, PROP_BITRATE,
            g_param_spec_int ("bitrate",
                "Bitrate",
                "Target test Bitrate (0 = fixed value based on "
                " sample rate and channel count)",
                0, G_MAXINT, 1000000,
                GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
}

static GstCaps*
gst_decklink_audio_src_get_caps (TfiSdiSrc* bsrc, GstCaps * filter) {
    printf("need caps\n");
    exit(0);
    return NULL;
}


static GstFlowReturn
tfi_sdi_src_alloc (TfiSdiSrc* src, guint64 offset, guint size, GstBuffer ** buffer)
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
    //GstPad *pad;
    src->pad_counter = 0;
    src->running = FALSE;
    //g_queue_init(&src->pad_queue);

    src->video_pad = gst_pad_new_from_static_template(&video_template, "video");
    src->audio_pad = gst_pad_new_from_static_template(&audio_template, "audio");


    /*
    GstCaps *caps2 = gst_caps_new_simple ("video/x-raw",
            "format", G_TYPE_STRING, "UYVY",
            "width", G_TYPE_INT, 1920,
            "height", G_TYPE_INT, 1080,
            NULL);

    if (gst_pad_set_caps(pad, caps2) == FALSE) {
        printf("yyFail to set caps\n");
        exit(0);
    }
    */
    gst_pad_set_query_function (src->video_pad, gst_base_src_query);
    gst_pad_set_query_function (src->audio_pad, gst_base_src_query);

    gst_pad_set_event_function (src->video_pad, gst_stream_event);
    gst_pad_set_event_function (src->audio_pad, gst_stream_event);

    gst_element_add_pad (GST_ELEMENT (src), src->video_pad);
    gst_element_add_pad (GST_ELEMENT (src), src->audio_pad);

    src->bitrate = 12345;
	fout = fopen("sdi.log", "wt");

    //g_queue_push_tail(&src->pad_queue, pad);


    TfiDecklinkClock *clk = (TfiDecklinkClock*)tfi_decklink_clock_new ("TfiDecklinkClock");
    clk->input = src;
    src->clock = (GstClock*)clk;

}

/*
static GstFlowReturn create2 (GstPad *pad, GstBuffer ** buffer)
{
    GstFlowReturn ret;
    int blocksize = ((long int)pad) & 0x00ffffff;
    TfiSdiSrc *src = TFI_SDI_SRC(GST_OBJECT_PARENT (pad));

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
            GstPad *pad = (GstPad*)g_queue_peek_nth (&basesrc->pad_queue, i);
            g_thread_new("padthread", &workpad, pad);
        }
        sleep(1);
    }
    return NULL;
}
*/

typedef struct
{
    IDeckLinkVideoInputFrame *frame;
} VideoFrame;

static void
video_frame_free (void *data)
{
    VideoFrame *frame = (VideoFrame *) data;
    frame->frame->Release();
    g_free(frame);
}

typedef struct
{
    IDeckLinkAudioInputPacket *packet;
} AudioPacket;

static void
audio_packet_free (void *data)
{
    AudioPacket *packet = (AudioPacket *) data;
    packet->packet->Release ();
    g_free(packet);
}

typedef struct
{
    GstPad * pad;
    GAsyncQueue *q;
} Context;

static void* workPad(void* p) 
{
    Context *context = (Context*)p;
    while(1) {
        gpointer buf = NULL;
        gint size = g_async_queue_length(context->q);
        //printf("hello:%p | size:%d\n", context->pad, size);

        buf = g_async_queue_try_pop(context->q);
        if (buf) {
            gst_pad_push(context->pad, (GstBuffer*)buf);
        } else {
            sleep(0.01);
        }
    }
}


class InputCallback : public IDeckLinkInputCallback
{
    private:
        GMutex m_mutex;
        uint32_t m_refCount;
        uint32_t m_frameCnt;
        IDeckLinkInput*	m_deckLinkInput;
        IDeckLinkDisplayMode* m_displayMode;
        BMDConfig m_config;
        uint64_t m_pts;
        bool m_bSetupVideo;
        bool m_bSetupAudio;
        bool m_bCanStart;
    private:
        //GstPad *m_pad;
        GstPad *m_videoPad;
        GstPad *m_audioPad;

        Context m_videoContext;
        Context m_audioContext;

    public:
        InputCallback(IDeckLinkInput *deckLinkInput, IDeckLinkDisplayMode *displayMode, const BMDConfig &config)
            :m_refCount(0),m_frameCnt(0),m_deckLinkInput(deckLinkInput), 
            m_displayMode(displayMode), m_config(config), m_pts(0) 
        {
            m_bSetupVideo = false;
            m_bSetupAudio = false;
            m_bCanStart = false;

        }

        virtual HRESULT STDMETHODCALLTYPE QueryInterface (REFIID, LPVOID *)
        {
            return E_NOINTERFACE;
        }

        virtual ULONG STDMETHODCALLTYPE AddRef (void)
        {
            return __sync_add_and_fetch(&m_refCount, 1);
        }

        virtual ULONG STDMETHODCALLTYPE Release (void)
        {
            int32_t newRefValue = __sync_sub_and_fetch(&m_refCount, 1);
            if (newRefValue == 0)
            {
                delete this;
                return 0;
            }
            return newRefValue;
        }

        void setPads(GstPad *vP, GstPad *aP) {
            m_videoPad = vP;
            m_audioPad = aP;
            m_videoContext.pad = vP;
            m_audioContext.pad = aP;
            m_videoContext.q = g_async_queue_new();
            m_audioContext.q = g_async_queue_new();

            gst_pad_start_task(m_videoPad, (GstTaskFunction)workPad, &m_videoContext, NULL);
            gst_pad_start_task(m_audioPad, (GstTaskFunction)workPad, &m_audioContext, NULL);
        }

        virtual HRESULT STDMETHODCALLTYPE
            VideoInputFormatChanged (BMDVideoInputFormatChangedEvents,
                    IDeckLinkDisplayMode * mode, BMDDetectedVideoInputFormatFlags formatFlags)
            {
                // This only gets called if bmdVideoInputEnableFormatDetection was set
                // when enabling video input
                HRESULT	result;
                char*	displayModeName = NULL;
                BMDPixelFormat	pixelFormat = bmdFormat8BitYUV;

                mode->GetName((const char**)&displayModeName);
                fprintf(LOG_HDL, "Video format changed to %s %s | %ldx%ld\n",
                        displayModeName, formatFlags & bmdDetectedVideoInputRGB444 ? "RGB" : "YUV",
                        mode->GetWidth(), mode->GetHeight());

                GstCaps *caps = gst_caps_new_simple ("video/x-raw",
                                "format", G_TYPE_STRING, "UYVY",
                                "width", G_TYPE_INT, mode->GetWidth(),
                                "height", G_TYPE_INT, mode->GetHeight(), 
                                "framerate", GST_TYPE_FRACTION, 25, 1,
                                NULL);

                if (gst_pad_set_caps(m_videoPad, caps) == FALSE) {
                    printf("Fail to set video caps\n");
                    exit(0);
                }

                /*
                GstCaps *audio_caps = gst_caps_new_simple ("audio/x-raw",
                                "format", G_TYPE_STRING, "S16LE",
                                "channels", G_TYPE_INT, 2,
                                "rate", G_TYPE_INT, 48000,
                                NULL);

                if (gst_pad_set_caps(m_audioPad, audio_caps) == FALSE) {
                    printf("Fail to set audio caps\n");
                    exit(0);
                }
                */

                if (displayModeName)
                    free(displayModeName);

                if (m_deckLinkInput)
                {
                    m_deckLinkInput->StopStreams();
                    result = m_deckLinkInput->EnableVideoInput(
                            mode->GetDisplayMode(), pixelFormat, m_config.m_inputFlags);

                    result = m_deckLinkInput->EnableAudioInput(
                            bmdAudioSampleRate48kHz,
                            bmdAudioSampleType16bitInteger, 2);

                    if (result != S_OK) {
                        fprintf(stderr, "Failed to switch video mode\n");
                    } else {
                        m_deckLinkInput->StartStreams();
                    }
                }

                return S_OK;

            }

        virtual HRESULT STDMETHODCALLTYPE
            VideoInputFrameArrived (IDeckLinkVideoInputFrame * video_frame,
                    IDeckLinkAudioInputPacket * audio_packet)
            {
                bool bGoodVideoFrm = video_frame->GetFlags() & bmdFrameHasNoInputSource ? false : true;
                if (!m_bCanStart) {
                    m_bCanStart = bGoodVideoFrm;
                }

                if (video_frame) {
                    if (!bGoodVideoFrm) {
                        fprintf(LOG_HDL, "GotFrame(#%d): No input signal detected\n", m_frameCnt);
                    }
                    else {
                        GstBuffer *buffer = NULL;
                        BMDTimeValue stream_time = GST_CLOCK_TIME_NONE;
                        BMDTimeValue stream_duration = GST_CLOCK_TIME_NONE; 
                        int32_t width = video_frame->GetRowBytes();
                        int32_t height = video_frame->GetHeight();
                        int32_t bufsize = width * height;
                        void* buf;


                        video_frame->GetStreamTime (&stream_time, &stream_duration, GST_SECOND);

                        video_frame->GetBytes(&buf);
                        VideoFrame *vf = (VideoFrame *) g_malloc0 (sizeof (VideoFrame));

                        buffer = gst_buffer_new_wrapped_full ((GstMemoryFlags) GST_MEMORY_FLAG_READONLY,
                                (gpointer) buf, bufsize, 0, bufsize, vf,
                                (GDestroyNotify) video_frame_free);

                        //GST_BUFFER_PTS(buffer) = stream_time +  GST_SECOND * 60 * 60 * 1000;
                        GST_BUFFER_PTS(buffer) = stream_time;
                        GST_BUFFER_DURATION(buffer) = stream_duration;


                        vf->frame = video_frame;
                        vf->frame->AddRef();

                        if (!m_bSetupVideo) {
                            fprintf(LOG_HDL, "\n\n\t\tsetting segment to pad\n");
                            GstSegment *segment = gst_segment_new();
                            gst_segment_init(segment, GST_FORMAT_TIME);
                            segment->base = basetime;
                            //segment->start = GST_SECOND * 60 * 60 * 1000;
                            gst_pad_push_event (m_videoPad, gst_event_new_segment (segment));
                            gst_segment_free(segment);
                            m_bSetupVideo = true;
                        }

                        g_async_queue_push (m_videoContext.q, buffer);

                        //gst_pad_push(m_videoPad, buffer);
                        buffer = NULL;
                    }
                    m_frameCnt++;
                }

                if (audio_packet && m_bCanStart)
                {
                    GstBuffer *buffer = NULL;
                    int32_t sample_count = audio_packet->GetSampleFrameCount();
                    BMDTimeValue stream_time = GST_CLOCK_TIME_NONE;
                    BMDTimeValue stream_duration = GST_CLOCK_TIME_NONE; 
                    void* buf;
                    int32_t bufsize = sample_count * 2 * 2;
                    audio_packet->GetBytes(&buf);
                    AudioPacket *ap = (AudioPacket *) g_malloc0 (sizeof (AudioPacket));

                    video_frame->GetStreamTime (&stream_time, &stream_duration, GST_SECOND);

                    buffer = gst_buffer_new_wrapped_full ((GstMemoryFlags) GST_MEMORY_FLAG_READONLY,
                            (gpointer) buf, bufsize, 0, bufsize, ap,
                            (GDestroyNotify) audio_packet_free);

                    ap->packet = audio_packet;
                    ap->packet->AddRef();

                    //GST_BUFFER_PTS(buffer) = stream_time +  GST_SECOND * 60 * 60 * 1000;
                    GST_BUFFER_PTS(buffer) = stream_time;
                    GST_BUFFER_DURATION(buffer) = stream_duration;
                    //GST_BUFFER_OFFSET(buffer) = 0;
                    //GST_BUFFER_OFFSET_END(buffer) = sample_count;

                    if (!m_bSetupAudio) {
                        fprintf(LOG_HDL, "\n\n\t\tsetting segment to audio pad\n");
                        GstSegment *segment = gst_segment_new();
                        gst_segment_init(segment, GST_FORMAT_TIME);
                        segment->base = basetime;
                        //segment->start = GST_SECOND * 40 / 1000;
                        //segment->start = GST_SECOND * 2;
                        //segment->start = GST_SECOND * 10 * 1000;
                        //segment->start = GST_SECOND * 60 * 60 * 1000;
                        gst_pad_push_event (m_audioPad, gst_event_new_segment (segment));
                        gst_segment_free(segment);
                        m_bSetupAudio = true;

                    }

                    g_async_queue_push (m_audioContext.q, buffer);
                    //gst_pad_push(m_audioPad, buffer);
                    buffer = NULL;
                }

				fflush(fout);
                return S_OK;
            }

};

static IDeckLink* GetDeckLink(int deviceIdx) 
{
	HRESULT	result;
	IDeckLinkIterator* deckLinkIterator = NULL;
	IDeckLink* deckLink = NULL;
    int idx = deviceIdx;

	deckLinkIterator = CreateDeckLinkIteratorInstance();
	if (!deckLinkIterator) {
		fprintf(stderr, "This application requires the DeckLink drivers installed.\n");
        return NULL;
	}

	while ((result = deckLinkIterator->Next(&deckLink)) == S_OK) {
		if (idx== 0)
			break;
		--idx;
		deckLink->Release();
	}

	if (result != S_OK || deckLink == NULL) {
		fprintf(stderr, "Unable to get DeckLink device %u\n", deviceIdx);
	}

	if (deckLinkIterator != NULL) {
		deckLinkIterator->Release();
    }

    return deckLink;
}

static bool IsFormatDetectionSupported(IDeckLink *deckLink) 
{
	IDeckLinkAttributes* deckLinkAttributes = NULL;
	bool formatDetectionSupported = false;
    bool rv = false;

    HRESULT	result = deckLink->QueryInterface(IID_IDeckLinkAttributes, (void**)&deckLinkAttributes);
    if (result == S_OK) {
        result = deckLinkAttributes->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &formatDetectionSupported);
        if (result != S_OK || !formatDetectionSupported) {
            fprintf(stderr, "Format detection is not supported on this device\n");
        } else {
            rv = true; // only here is good
        }
    }

    if (deckLinkAttributes != NULL)
		deckLinkAttributes->Release();

    return rv;
}

static IDeckLinkDisplayMode* GetDisplayMode(IDeckLinkInput *deckLinkInput, int modeIdx)
{
	IDeckLinkDisplayMode* displayMode = NULL;
	IDeckLinkDisplayModeIterator* displayModeIterator = NULL;
    int idx = modeIdx;

	HRESULT	result = deckLinkInput->GetDisplayModeIterator(&displayModeIterator);
	if (result != S_OK)
        return NULL;

	while ((result = displayModeIterator->Next(&displayMode)) == S_OK)
	{
		if (idx == 0)
			break;
		--idx;
		displayMode->Release();
	}

	if (result != S_OK || displayMode == NULL) {
        fprintf(stderr, "Unable to get display mode %d\n", modeIdx);
        if (displayMode) {
            displayMode->Release();
            displayMode = NULL;
        }
	}

	if (displayModeIterator != NULL)
		displayModeIterator->Release();

    return displayMode;
}

static bool 
IsVideoModeSupported(IDeckLinkInput *deckLinkInput, IDeckLinkDisplayMode *displayMode, const BMDConfig &config)		
{
    bool rv = false;
	char* displayModeName = NULL;
	HRESULT	result;
	BMDDisplayModeSupport displayModeSupported;

	result = displayMode->GetName((const char**)&displayModeName);
	if (result != S_OK) {
		displayModeName = (char *)malloc(32);
		snprintf(displayModeName, 32, "[index %d]", config.m_displayModeIndex);
	}

	// Check display mode is supported with given options
    result = deckLinkInput->DoesSupportVideoMode(
            displayMode->GetDisplayMode(), 
            config.m_pixelFormat, 
            bmdVideoInputFlagDefault, 
            &displayModeSupported, NULL);

	if (result != S_OK || displayModeSupported == bmdDisplayModeNotSupported) {
		fprintf(stderr, "The display mode %s is not supported with the selected pixel format\n", displayModeName);
	} else {
        rv = true;
    }

	if (displayModeName != NULL)
		free(displayModeName);

    return rv;
}

static bool start(TfiSdiSrc *src) {
    //src->datathread = g_thread_new("datathread", &work, src);
	HRESULT	result;
    int idx;
    BMDConfig config;
    InputCallback *cb;

    //testclock = src->clock;
    testclock = gst_element_get_clock ((GstElement*)src);
    basetime = gst_element_get_base_time((GstElement*)src);


    
    /*
    while(1) {
        GstClockTime capture_time = gst_clock_get_time(testclock);
        GstClockTime base_time = gst_element_get_base_time((GstElement*)src);
        GstClockTime offset_time = capture_time - base_time;
        g_print("%ld\t%ld\tdiff:%ld\n", 
                GST_TIME_AS_MSECONDS(base_time), 
                GST_TIME_AS_MSECONDS(capture_time),
                GST_TIME_AS_MSECONDS(offset_time));
        sleep(1);
    }
    */


    // Get the decklink device instance and (capture interface)
    IDeckLink *deckLink = GetDeckLink(0);
    result = deckLink->QueryInterface(IID_IDeckLinkInput, (void**)&src->deckLinkInput);
    if (result != S_OK)
        goto fail;

    // Check the card supports format detection
    if (IsFormatDetectionSupported(deckLink)) {
        config.m_inputFlags |= bmdVideoInputEnableFormatDetection;
    } else {
        goto fail;
    }

    // Get the display mode
    idx = (config.m_displayModeIndex == -1) ? 0 : config.m_displayModeIndex;
    src->displayMode = GetDisplayMode(src->deckLinkInput, idx);

    if (src->displayMode == NULL) {
        goto fail;
    }

    if (!IsVideoModeSupported(src->deckLinkInput, src->displayMode, config)) {
        goto fail;
    }

    config.DisplayConfiguration();

    cb = new InputCallback(src->deckLinkInput, src->displayMode, config);
    cb->setPads(src->video_pad, src->audio_pad);

    {
        GstCaps *caps = NULL;
        GstEvent *event = gst_event_new_stream_start ("test");
        gboolean ret = gst_pad_push_event (src->audio_pad, event);

        GstCaps *thiscaps = gst_pad_query_caps (src->audio_pad, NULL);
        GST_DEBUG_OBJECT (src, "caps of src: %" GST_PTR_FORMAT, thiscaps);
        if (thiscaps == NULL || gst_caps_is_any (thiscaps))
        {
            printf("fuck\n");
            exit(0);
        }

        if (G_UNLIKELY (gst_caps_is_empty (thiscaps)))
        {
            printf("fuck2\n");
            exit(0);
        }

        GstCaps *peercaps = gst_pad_peer_query_caps (src->audio_pad, thiscaps);
        GST_DEBUG_OBJECT (src, "caps of peer: %" GST_PTR_FORMAT, peercaps);
        if (peercaps) {
            caps = peercaps;
            gst_caps_unref (thiscaps);
        } else {
            caps = thiscaps;
        }

        if (caps && !gst_caps_is_empty (caps)) {
            GST_DEBUG_OBJECT (src, "have caps: %" GST_PTR_FORMAT, caps);
            if (gst_caps_is_any (caps)) {
                GST_DEBUG_OBJECT (src, "any caps, we stop");
                result = TRUE;
            } else {
                gst_caps_fixate(caps);
                if (gst_caps_is_fixed (caps)) {
                    gst_pad_push_event (src->audio_pad, gst_event_new_caps (caps));
                }
            }
            gst_caps_unref (caps);
        } else {
            if (caps)
                gst_caps_unref (caps);
            GST_DEBUG_OBJECT (src, "no common caps");
        }


        caps = gst_pad_get_current_caps (src->audio_pad);
        GstQuery *query = gst_query_new_allocation (caps, TRUE);
        if (!gst_pad_peer_query (src->audio_pad, query)) {
            g_print("peer query fail\n");
        }
    }



    /*
    {
        gint i;
        gint length = g_queue_get_length(&src->pad_queue);
        for (i = 0; i < length; i++) {
            GstPad *pad = (GstPad*)g_queue_peek_nth (&src->pad_queue, i);
            cb->m_pad = pad;
            break;
        }
    }
    */
	src->deckLinkInput->SetCallback(cb); // deckLinkInput take the callback obj ownership

    src->running = TRUE;

    gst_element_post_message (GST_ELEMENT_CAST (src), gst_message_new_latency (GST_OBJECT_CAST (src)));

    // Start capturing
    result = src->deckLinkInput->EnableVideoInput(src->displayMode->GetDisplayMode(), 
            config.m_pixelFormat, config.m_inputFlags);

    result = src->deckLinkInput->EnableAudioInput(bmdAudioSampleRate48kHz,
                  bmdAudioSampleType16bitInteger, 2);


    if (result != S_OK) {
        fprintf(stderr, "Failed to enable video input. Is another application using the card?\n");
        goto fail;
    }

    result = src->deckLinkInput->EnableAudioInput(bmdAudioSampleRate48kHz, 
            config.m_audioSampleDepth, config.m_audioChannels);
    if (result != S_OK)
        goto fail;

    result = src->deckLinkInput->StartStreams();
    if (result != S_OK)
        goto fail;

    return true;
fail:
    if (deckLink != NULL)
		deckLink->Release();

    return false;

}

static void stop (TfiSdiSrc *src)
{
    src->deckLinkInput->StopStreams();
    src->deckLinkInput->DisableAudioInput();
    src->deckLinkInput->DisableVideoInput();
    
	if (src->deckLinkInput != NULL) {
		src->deckLinkInput->Release();
    }

	if (src->displayMode != NULL) {
		src->displayMode->Release();
    }

	fflush(fout);
}

static GstStateChangeReturn
gst_base_src_change_state (GstElement * element, GstStateChange transition)
{
    TfiSdiSrc *src = TFI_SDI_SRC (element);
    GstStateChangeReturn result;
    gboolean no_preroll = FALSE;

    switch (transition) {
        case GST_STATE_CHANGE_NULL_TO_READY:
            fprintf(LOG_HDL, "TFI:GST_STATE_CHANGE_NULL_TO_READY\n");
            g_mutex_lock (&src->lock);
            src->clock_start_time = GST_CLOCK_TIME_NONE;
            src->clock_epoch += src->clock_last_time;
            src->clock_last_time = 0;
            src->clock_offset = 0;
            g_mutex_unlock (&src->lock);
            gst_element_post_message (element,
                    gst_message_new_clock_provide (GST_OBJECT_CAST (element),
                        src->clock, TRUE));
            break;
        case GST_STATE_CHANGE_READY_TO_PAUSED:
            fprintf(LOG_HDL, "TFI:GST_STATE_CHANGE_READY_TO_PAUSED\n");
            no_preroll = TRUE;
            break;
        case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
            fprintf(LOG_HDL, "TFI:GST_STATE_CHANGE_PAUSED_TO_PLAYING\n");
            if (!start(src)) {
                g_print("Fail to start capturing\n");
            }
            break;
        default:
            break;
    }

	fflush(fout);

    if ((result = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition)) 
            == GST_STATE_CHANGE_FAILURE)
        goto failure;

    switch (transition) {
        case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
            {
                no_preroll = TRUE;
                stop(src);
            }
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



G_DEFINE_TYPE (TfiDecklinkClock, tfi_decklink_clock, GST_TYPE_SYSTEM_CLOCK);

static void
tfi_decklink_clock_class_init (TfiDecklinkClockClass * klass)
{
    GstClockClass *clock_class = (GstClockClass *) klass;
    clock_class->get_internal_time = tfi_decklink_clock_get_internal_time;
}

static void
tfi_decklink_clock_init (TfiDecklinkClock * clock)
{
    GST_OBJECT_FLAG_SET (clock, GST_CLOCK_FLAG_CAN_SET_MASTER);
}

static GstClock *
tfi_decklink_clock_new (const gchar * name)
{
    TfiDecklinkClock *self =
        GST_DECKLINK_CLOCK (g_object_new (GST_TYPE_TFI_DECKLINK_CLOCK, "name", name,
                    "clock-type", GST_CLOCK_TYPE_OTHER, NULL));
    gst_object_ref_sink (self);
    return GST_CLOCK_CAST (self);
}

static GstClockTime
tfi_decklink_clock_get_internal_time (GstClock * clock)
{
    TfiDecklinkClock *self = (TfiDecklinkClock*)(clock);
    GstClockTime result, start_time, last_time;
    GstClockTimeDiff offset;
    BMDTimeValue time;
    HRESULT ret;

    if (self->input->running == FALSE)
        return 0;

    g_mutex_lock (&self->input->lock);
    start_time = self->input->clock_start_time;
    offset = self->input->clock_offset;
    last_time = self->input->clock_last_time;
    time = -1;
    ret = self->input->deckLinkInput->GetHardwareReferenceClock (GST_SECOND, &time, NULL, NULL);

    if (ret == S_OK && time >= 0) {
        result = time;

        if (start_time == GST_CLOCK_TIME_NONE)
            start_time = self->input->clock_start_time = result;

        if (result > start_time)
            result -= start_time;
        else
            result = 0;

        result = MAX (last_time, result);
        result -= offset;
        result = MAX (last_time, result);
    } else {
        result = last_time;
    }

    self->input->clock_last_time = result;
    result += self->input->clock_epoch;
    g_mutex_unlock (&self->input->lock);

    return result;
}

static GstClock *gst_tfi_sdi_src_provide_clock (GstElement * element)
{
    TfiSdiSrc *self = (TfiSdiSrc*)(element);
    return GST_CLOCK_CAST (gst_object_ref (self->clock));
}

static gboolean
gst_stream_event (GstPad * pad, GstObject * parent, GstEvent * event) {
    gboolean ret = FALSE;
    switch (GST_EVENT_TYPE (event)) {
        case GST_EVENT_LATENCY:
            {
                GstClockTime latency;
                gst_event_parse_latency (event, &latency);
                fprintf(LOG_HDL, "New latency receive [%s] latency:%ld\n", gst_pad_get_name(pad), latency);
            }
            break;
        default:
            ret = gst_pad_event_default (pad, parent, event);
    }
    return ret;
}

static gboolean
gst_base_src_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
    switch (GST_QUERY_TYPE (query)) {
        case GST_QUERY_CAPS:
            {
                GstCaps *caps = gst_pad_get_pad_template_caps(pad);
                gst_query_set_caps_result(query, caps);
                gst_caps_unref(caps);
            }
            break;
        case GST_QUERY_LATENCY:
            {
                /*
                gboolean live = FALSE;
                GstClockTime min_latency = 0;
                GstClockTime max_latency = 0;

                gboolean res = gst_pad_peer_query (pad, query);
                if (res) {
                    gst_query_parse_latency (query, &live, &min_latency, &max_latency);
                    printf("\tLatency [%s] live:%d min:%ld max:%ld\n", gst_pad_get_name(pad), live, 
                            min_latency, max_latency);
                }
                */

                //GstClockTime min = gst_util_uint64_scale_ceil (GST_SECOND, 1, 25);
                //GstClockTime max = min * 1;
                GstClockTime min = 0;
                GstClockTime max = 0;
                gst_query_set_latency(query, TRUE, min, max); // todo: what should we report?

                /*
                GstSegment *segment = gst_segment_new();
                gst_segment_init(segment, GST_FORMAT_TIME);
                segment->base = basetime;
                segment->start = max_latency;
                gst_pad_push_event (pad, gst_event_new_segment (segment));
                gst_segment_free(segment);
                */

                //gst_query_set_latency(query, TRUE, 0, 0); // todo: what should we report?
            }
            break;
        case GST_QUERY_DURATION:
            {
                GstFormat format;
                gst_query_parse_duration (query, &format, NULL);
                gst_query_set_duration (query, format, -1);
            }
            break;
        default:
            gst_pad_query_default(pad, parent, query);
    }
    return true;
}

