/*
 * audio_null_ios.c — minimal Wine audio "null" driver for iOS Mythic.
 *
 * Wine's mmdevapi loads a `wine<name>.drv` PE plus a unix-side function
 * table (37 entries). On Linux/macOS the unix table is a separate .so.
 * On iOS we statically link the table into Mythic.app — this file is
 * that table for "ios" / "coreaudio".
 *
 * Behaviour: ONE fake render endpoint, accepts buffer submissions and
 * discards, advances IAudioClock at real-time based on
 * mach_absolute_time. Enough to let FMOD's clock-driven timing
 * advance (rhythm games like Thumper gate splash→title on intro
 * music completing — this is what makes that work).
 *
 * NOT real audio output. Tier-1 silent driver. Tier-2 would route
 * buffers to AVAudioEngine for actual playback; defer until visible
 * progress past splash confirms this is the right path.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach_time.h>
#include <unistd.h>

/* Struct/enum mirrors from wine/dlls/mmdevapi/unixlib.h. Repeating the
 * essential layout here avoids include-path drama with Wine's COM
 * headers, which pull in <objbase.h>/<audioclient.h>. We only need the
 * struct fields the unix-call dispatch touches. */

typedef int NTSTATUS;
typedef uint16_t WCHAR;
typedef int32_t HRESULT;
typedef uint32_t DWORD;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef uint64_t UINT_PTR;
typedef uint32_t UINT;
typedef int BOOL;
typedef uint8_t BYTE;
typedef int64_t REFERENCE_TIME;
typedef void *HANDLE;
typedef uint16_t WORD;
typedef uint64_t stream_handle;
typedef int EDataFlow;

#define STATUS_SUCCESS 0
#define S_OK 0
#define S_FALSE 1
#define E_FAIL 0x80004005L
#define AUDCLNT_E_NOT_INITIALIZED 0x88890001L

#define eRender 0
#define eCapture 1

enum driver_priority {
    Priority_Unavailable = 0,
    Priority_Low,
    Priority_Neutral,
    Priority_Preferred
};

struct endpoint {
    unsigned int name;
    unsigned int device;
};

struct main_loop_params { HANDLE event; };

struct get_endpoint_ids_params {
    EDataFlow flow;
    struct endpoint *endpoints;
    unsigned int size;
    HRESULT result;
    unsigned int num;
    unsigned int default_idx;
};

struct WAVEFORMATEX_stub {
    WORD wFormatTag;
    WORD nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD nBlockAlign;
    WORD wBitsPerSample;
    WORD cbSize;
};

struct create_stream_params {
    const WCHAR *name;
    const char *device;
    EDataFlow flow;
    int share;
    DWORD flags;
    REFERENCE_TIME duration;
    REFERENCE_TIME period;
    const struct WAVEFORMATEX_stub *fmt;
    HRESULT result;
    UINT32 *channel_count;
    stream_handle *stream;
};

struct stream_handle_params { stream_handle stream; HRESULT result; };
struct stream_handle_only { stream_handle stream; };

struct release_stream_params {
    stream_handle stream;
    HANDLE timer_thread;
    HRESULT result;
};

struct get_render_buffer_params {
    stream_handle stream;
    UINT32 frames;
    HRESULT result;
    BYTE **data;
};

struct release_render_buffer_params {
    stream_handle stream;
    UINT32 written_frames;
    UINT flags;
    HRESULT result;
};

struct get_capture_buffer_params {
    stream_handle stream;
    HRESULT result;
    BYTE **data;
    UINT32 *frames;
    UINT *flags;
    UINT64 *devpos;
    UINT64 *qpcpos;
};

struct release_capture_buffer_params {
    stream_handle stream;
    UINT32 done;
    HRESULT result;
};

struct is_format_supported_params {
    const char *device;
    EDataFlow flow;
    int share;
    const struct WAVEFORMATEX_stub *fmt_in;
    HRESULT result;
};

struct get_mix_format_params {
    const char *device;
    EDataFlow flow;
    void *fmt;          /* WAVEFORMATEXTENSIBLE */
    HRESULT result;
};

struct get_device_period_params {
    const char *device;
    EDataFlow flow;
    HRESULT result;
    REFERENCE_TIME *def_period;
    REFERENCE_TIME *min_period;
};

struct get_buffer_size_params {
    stream_handle stream;
    HRESULT result;
    UINT32 *frames;
};

struct get_latency_params {
    stream_handle stream;
    HRESULT result;
    REFERENCE_TIME *latency;
};

struct get_current_padding_params {
    stream_handle stream;
    HRESULT result;
    UINT32 *padding;
};

struct get_next_packet_size_params {
    stream_handle stream;
    HRESULT result;
    UINT32 *frames;
};

struct get_frequency_params {
    stream_handle stream;
    HRESULT result;
    UINT64 *freq;
};

struct get_position_params {
    stream_handle stream;
    BOOL device;
    HRESULT result;
    UINT64 *pos;
    UINT64 *qpctime;
};

struct set_volumes_params {
    stream_handle stream;
    float master_volume;
    const float *volumes;
    const float *session_volumes;
};

struct set_event_handle_params {
    stream_handle stream;
    HANDLE event;
    HRESULT result;
};

struct set_sample_rate_params {
    stream_handle stream;
    float rate;
    HRESULT result;
};

struct test_connect_params {
    const WCHAR *name;
    enum driver_priority priority;
};

struct is_started_params {
    stream_handle stream;
    HRESULT result;
};

struct get_prop_value_params {
    const char *device;
    EDataFlow flow;
    const void *guid;
    const void *prop;
    HRESULT result;
    void *value;
    void *buffer;
    unsigned int *buffer_size;
};

/* ---------------------------------------------------------------- */

#define IOS_AUDIO_SAMPLE_RATE 48000u
#define IOS_AUDIO_CHANNELS 2u
#define IOS_AUDIO_BITS 16u
#define IOS_AUDIO_FRAME_BYTES ((IOS_AUDIO_CHANNELS * IOS_AUDIO_BITS) / 8u) /* 4 */
#define IOS_AUDIO_BUFFER_FRAMES 1024u  /* ~21 ms at 48 kHz */
#define IOS_AUDIO_BUFFER_BYTES (IOS_AUDIO_BUFFER_FRAMES * IOS_AUDIO_FRAME_BYTES)

/* The "device" Wine probes by name. mmdevapi stores it on the endpoint
 * struct and passes it back as `const char *device` in many calls. */
static const char IOS_DEVICE_NAME[] = "ios-null";

/* One global stream state — single render endpoint, single stream. FMOD
 * typically creates one shared-mode render stream; if a game opens a
 * second concurrent stream we'd need a table. Not worried about that
 * for the Tier-1 silent driver. */
struct ios_stream {
    int valid;
    int started;
    uint64_t start_mach;        /* mach_absolute_time() at start() */
    uint64_t accumulated_frames; /* total frames "played" before last stop */
    UINT32 sample_rate;
    UINT32 channels;
    UINT32 buffer_frames;
    BYTE *render_scratch;        /* discard buffer for GetRenderBuffer */
    HANDLE event;
};

static struct ios_stream g_stream;
static mach_timebase_info_data_t g_timebase;

/* Per-function call counters. Print every 1000 calls so we can confirm
 * FMOD is actually exercising the driver. Cheap atomic increments. */
#include <stdatomic.h>
#define NULL_AUDIO_FN_COUNT 37
static _Atomic uint32_t g_call_counter[NULL_AUDIO_FN_COUNT];
#define LOG_FN_CALL(idx, name) do { \
    uint32_t n = atomic_fetch_add_explicit(&g_call_counter[idx], 1, memory_order_relaxed) + 1; \
    if (n == 1 || (n % 1000) == 0) { \
        char buf[128]; \
        int len = snprintf(buf, sizeof(buf), "[ios_audio] " name " #%u\n", n); \
        if (len > 0) write(STDERR_FILENO, buf, len); \
    } \
} while (0)

static uint64_t mach_to_ns(uint64_t mach) {
    if (!g_timebase.denom) mach_timebase_info(&g_timebase);
    return mach * g_timebase.numer / g_timebase.denom;
}

static uint64_t elapsed_ns_since(uint64_t mach_start) {
    return mach_to_ns(mach_absolute_time() - mach_start);
}

static uint64_t elapsed_frames(const struct ios_stream *s) {
    if (!s->started) return s->accumulated_frames;
    uint64_t ns = elapsed_ns_since(s->start_mach);
    /* frames = ns * rate / 1e9 */
    return s->accumulated_frames + (ns * s->sample_rate / 1000000000ull);
}

/* ---------------------------------------------------------------- */

static NTSTATUS ios_process_attach(void *args) {
    LOG_FN_CALL(0, "process_attach");
    (void)args;
    memset(&g_stream, 0, sizeof(g_stream));
    if (!g_timebase.denom) mach_timebase_info(&g_timebase);
    return STATUS_SUCCESS;
}

static NTSTATUS ios_process_detach(void *args) {
    (void)args;
    if (g_stream.render_scratch) {
        free(g_stream.render_scratch);
        g_stream.render_scratch = NULL;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS ios_main_loop(void *args) {
    (void)args;
    /* Real drivers spin here pumping the audio thread. We have nothing
     * to pump; mmdevapi calls this on a worker thread and we just
     * return so that worker dies cleanly. */
    return STATUS_SUCCESS;
}

static NTSTATUS ios_get_endpoint_ids(void *args) {
    LOG_FN_CALL(3, "get_endpoint_ids");
    struct get_endpoint_ids_params *p = args;
    /* Only render endpoints; refuse capture entirely. */
    if (p->flow != eRender) {
        p->num = 0;
        p->default_idx = 0;
        p->result = S_OK;
        return STATUS_SUCCESS;
    }
    /* mmdevapi treats endpoint.name as WCHAR* (wide string, 2 bytes/char)
     * and endpoint.device as char* (single-byte). Both stored as byte
     * offsets from the endpoints buffer base. */
    static const WCHAR dev_name_w[] = { 'i','O','S',' ','N','u','l','l', 0 };
    unsigned int name_bytes = sizeof(dev_name_w);
    unsigned int device_bytes = sizeof(IOS_DEVICE_NAME);
    unsigned int needed = sizeof(struct endpoint) + name_bytes + device_bytes;
    if (p->size < needed) {
        p->num = 1;
        p->default_idx = 0;
        p->result = 0x80070057L; /* E_INVALIDARG style — signal "need more space" */
        return STATUS_SUCCESS;
    }
    /* Layout: [endpoint][wide_name\0\0][device_str\0] */
    unsigned int name_off = sizeof(struct endpoint);
    unsigned int device_off = name_off + name_bytes;
    char *buf = (char *)p->endpoints;
    memcpy(buf + name_off, dev_name_w, name_bytes);
    memcpy(buf + device_off, IOS_DEVICE_NAME, device_bytes);
    p->endpoints[0].name = name_off;
    p->endpoints[0].device = device_off;
    p->num = 1;
    p->default_idx = 0;
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_create_stream(void *args) {
    LOG_FN_CALL(4, "create_stream");
    struct create_stream_params *p = args;
    /* Allocate or reuse the singleton stream. */
    g_stream.valid = 1;
    g_stream.started = 0;
    g_stream.start_mach = 0;
    g_stream.accumulated_frames = 0;
    g_stream.sample_rate = p->fmt ? p->fmt->nSamplesPerSec : IOS_AUDIO_SAMPLE_RATE;
    g_stream.channels = p->fmt ? p->fmt->nChannels : IOS_AUDIO_CHANNELS;
    g_stream.buffer_frames = IOS_AUDIO_BUFFER_FRAMES;
    if (!g_stream.render_scratch)
        g_stream.render_scratch = (BYTE *)calloc(1, IOS_AUDIO_BUFFER_BYTES * 4);
    if (p->channel_count) *p->channel_count = g_stream.channels;
    if (p->stream) *p->stream = (stream_handle)(uintptr_t)&g_stream;
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_release_stream(void *args) {
    struct release_stream_params *p = args;
    g_stream.valid = 0;
    g_stream.started = 0;
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_start(void *args) {
    LOG_FN_CALL(6, "start");
    struct stream_handle_params *p = args;
    if (!g_stream.started) {
        g_stream.start_mach = mach_absolute_time();
        g_stream.started = 1;
    }
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_stop(void *args) {
    struct stream_handle_params *p = args;
    if (g_stream.started) {
        g_stream.accumulated_frames = elapsed_frames(&g_stream);
        g_stream.started = 0;
    }
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_reset(void *args) {
    struct stream_handle_params *p = args;
    g_stream.started = 0;
    g_stream.accumulated_frames = 0;
    g_stream.start_mach = 0;
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_timer_loop(void *args) {
    (void)args;
    /* Real drivers spin signaling the event each period. We don't have a
     * source so we just return; mmdevapi's worker thread exits. FMOD's
     * own callback typically isn't dependent on this thread firing on
     * Windows. */
    return STATUS_SUCCESS;
}

static NTSTATUS ios_get_render_buffer(void *args) {
    LOG_FN_CALL(10, "get_render_buffer");
    struct get_render_buffer_params *p = args;
    if (p->data) *p->data = g_stream.render_scratch ? g_stream.render_scratch : (BYTE *)NULL;
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_release_render_buffer(void *args) {
    struct release_render_buffer_params *p = args;
    /* Pretend we played the frames — bump the accumulated count so the
     * clock keeps moving even when the game wants to push data faster
     * than the wall clock. */
    if (p->written_frames > 0 && g_stream.started) {
        /* No-op: real-time elapsed_frames() drives the clock already.
         * If we add to accumulated_frames here we'd race ahead of
         * wall clock and confuse rhythm-game pacing. */
    }
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_get_capture_buffer(void *args) {
    struct get_capture_buffer_params *p = args;
    if (p->frames) *p->frames = 0;
    if (p->data) *p->data = NULL;
    if (p->flags) *p->flags = 0;
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_release_capture_buffer(void *args) {
    struct release_capture_buffer_params *p = args;
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_is_format_supported(void *args) {
    struct is_format_supported_params *p = args;
    /* Accept anything. */
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_get_loopback_capture_device(void *args) {
    (void)args;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_get_mix_format(void *args) {
    struct get_mix_format_params *p = args;
    /* WAVEFORMATEXTENSIBLE is 40 bytes; first 18 are WAVEFORMATEX */
    if (p->fmt) {
        memset(p->fmt, 0, 40);
        struct WAVEFORMATEX_stub *f = p->fmt;
        f->wFormatTag = 0xFFFE; /* WAVE_FORMAT_EXTENSIBLE */
        f->nChannels = IOS_AUDIO_CHANNELS;
        f->nSamplesPerSec = IOS_AUDIO_SAMPLE_RATE;
        f->wBitsPerSample = IOS_AUDIO_BITS;
        f->nBlockAlign = IOS_AUDIO_FRAME_BYTES;
        f->nAvgBytesPerSec = IOS_AUDIO_SAMPLE_RATE * IOS_AUDIO_FRAME_BYTES;
        f->cbSize = 22; /* extensible body */
        /* Extensible body: Samples (2), ChannelMask (4), SubFormat (16).
         * KSDATAFORMAT_SUBTYPE_PCM = {00000001-0000-0010-8000-00AA00389B71} */
        uint16_t *samples = (uint16_t *)((char *)p->fmt + 18);
        *samples = IOS_AUDIO_BITS;
        uint32_t *mask = (uint32_t *)((char *)p->fmt + 20);
        *mask = 0x3; /* SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT */
        /* SubFormat GUID PCM */
        static const uint8_t pcm_guid[16] = {
            0x01,0x00,0x00,0x00, 0x00,0x00, 0x10,0x00,
            0x80,0x00, 0x00,0xAA, 0x00,0x38,0x9B,0x71
        };
        memcpy((char *)p->fmt + 24, pcm_guid, 16);
    }
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_get_device_period(void *args) {
    struct get_device_period_params *p = args;
    if (p->def_period) *p->def_period = 100000; /* 10 ms in 100ns units */
    if (p->min_period) *p->min_period = 50000;  /* 5 ms */
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_get_buffer_size(void *args) {
    struct get_buffer_size_params *p = args;
    if (p->frames) *p->frames = IOS_AUDIO_BUFFER_FRAMES;
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_get_latency(void *args) {
    struct get_latency_params *p = args;
    if (p->latency) *p->latency = 100000; /* 10 ms */
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_get_current_padding(void *args) {
    LOG_FN_CALL(20, "get_current_padding");
    struct get_current_padding_params *p = args;
    /* Always claim no backlog — ready for more data. FMOD will fill
     * buffers as fast as it wants; release_render_buffer discards. */
    if (p->padding) *p->padding = 0;
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_get_next_packet_size(void *args) {
    struct get_next_packet_size_params *p = args;
    if (p->frames) *p->frames = 0;
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_get_frequency(void *args) {
    struct get_frequency_params *p = args;
    /* Returns the device frequency in Hz — what units IAudioClock uses. */
    if (p->freq) *p->freq = g_stream.sample_rate ? g_stream.sample_rate : IOS_AUDIO_SAMPLE_RATE;
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_get_position(void *args) {
    LOG_FN_CALL(23, "get_position");
    struct get_position_params *p = args;
    /* THIS is the function that drives FMOD's clock. Returns the
     * "stream position" — number of frames since start. As long as this
     * advances at real time, FMOD thinks audio is playing and its
     * internal timing engine advances animations / events. */
    if (p->pos) *p->pos = elapsed_frames(&g_stream);
    if (p->qpctime) *p->qpctime = mach_to_ns(mach_absolute_time()) / 100; /* 100ns ticks */
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_set_volumes(void *args) {
    (void)args;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_set_event_handle(void *args) {
    struct set_event_handle_params *p = args;
    g_stream.event = p->event;
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_set_sample_rate(void *args) {
    struct set_sample_rate_params *p = args;
    if (p->rate > 0) g_stream.sample_rate = (UINT32)p->rate;
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_test_connect(void *args) {
    LOG_FN_CALL(27, "test_connect");
    struct test_connect_params *p = args;
    p->priority = Priority_Preferred;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_is_started(void *args) {
    struct is_started_params *p = args;
    p->result = g_stream.started ? S_OK : S_FALSE;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_get_prop_value(void *args) {
    struct get_prop_value_params *p = args;
    p->result = E_FAIL; /* property not supported — mmdevapi falls back */
    return STATUS_SUCCESS;
}

static NTSTATUS ios_midi_stub(void *args) {
    (void)args;
    return STATUS_SUCCESS;
}

/* Table indexed by enum unix_funcs in mmdevapi's unixlib.h (37 entries).
 * Order MUST match the enum exactly. */
const void *audio_null_ios_unix_call_funcs[] = {
    ios_process_attach,                /* process_attach */
    ios_process_detach,                /* process_detach */
    ios_main_loop,                     /* main_loop */
    ios_get_endpoint_ids,              /* get_endpoint_ids */
    ios_create_stream,                 /* create_stream */
    ios_release_stream,                /* release_stream */
    ios_start,                         /* start */
    ios_stop,                          /* stop */
    ios_reset,                         /* reset */
    ios_timer_loop,                    /* timer_loop */
    ios_get_render_buffer,             /* get_render_buffer */
    ios_release_render_buffer,         /* release_render_buffer */
    ios_get_capture_buffer,            /* get_capture_buffer */
    ios_release_capture_buffer,        /* release_capture_buffer */
    ios_is_format_supported,           /* is_format_supported */
    ios_get_loopback_capture_device,   /* get_loopback_capture_device */
    ios_get_mix_format,                /* get_mix_format */
    ios_get_device_period,             /* get_device_period */
    ios_get_buffer_size,               /* get_buffer_size */
    ios_get_latency,                   /* get_latency */
    ios_get_current_padding,           /* get_current_padding */
    ios_get_next_packet_size,          /* get_next_packet_size */
    ios_get_frequency,                 /* get_frequency */
    ios_get_position,                  /* get_position */
    ios_set_volumes,                   /* set_volumes */
    ios_set_event_handle,              /* set_event_handle */
    ios_set_sample_rate,               /* set_sample_rate */
    ios_test_connect,                  /* test_connect */
    ios_is_started,                    /* is_started */
    ios_get_prop_value,                /* get_prop_value */
    ios_midi_stub,                     /* midi_get_driver */
    ios_midi_stub,                     /* midi_init */
    ios_midi_stub,                     /* midi_release */
    ios_midi_stub,                     /* midi_out_message */
    ios_midi_stub,                     /* midi_in_message */
    ios_midi_stub,                     /* midi_notify_wait */
    ios_midi_stub,                     /* aux_message */
};
