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
 * 2026-07-05 TIER-2: REAL AUDIO OUTPUT via a RemoteIO AudioUnit.
 * WASAPI render semantics map onto a lock-free ring buffer:
 *   get_render_buffer  -> contiguous scratch pointer
 *   release_render_buffer -> copy scratch into the ring, advance write_pos
 *   RemoteIO render callback (Core Audio real-time thread — touches ONLY
 *   the ring + atomics, never Wine) -> copy ring to hardware, advance
 *   play_pos; underrun plays silence
 *   get_current_padding -> write_pos - play_pos
 *   get_position        -> play_pos (frames actually consumed)
 *   timer_loop          -> Wine thread; signals the client event per period
 * If AudioUnit setup fails (no session, etc.) the driver degrades to the
 * Tier-1 wall-clock null behaviour so game timing never breaks.
 * AVAudioSession activation happens app-side (WineProcessBridge.m).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach_time.h>
#include <unistd.h>
#include <AudioToolbox/AudioToolbox.h>

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
    uint64_t start_mach;        /* mach_absolute_time() at start() (null-mode clock) */
    uint64_t accumulated_frames; /* null-mode: frames "played" before last stop */
    UINT32 sample_rate;
    UINT32 channels;
    UINT32 frame_bytes;          /* nBlockAlign of the stream format */
    UINT32 buffer_frames;        /* ring capacity in frames */
    BYTE *render_scratch;        /* contiguous area handed to GetBuffer */
    UINT32 scratch_frames;       /* scratch capacity */
    UINT32 pending_frames;       /* frames handed out, awaiting release */
    HANDLE event;
    /* Tier-2 real output */
    AudioUnit au;                /* RemoteIO; NULL = null-mode fallback */
    int au_running;
    BYTE *ring;
    _Atomic uint64_t write_pos;  /* frames produced by the game (monotonic) */
    _Atomic uint64_t play_pos;   /* frames consumed by the RT callback */
};

static struct ios_stream g_stream;
static mach_timebase_info_data_t g_timebase;

/* NtSetEvent lives in the same statically-linked unix ntdll. timer_loop
 * runs on a real Wine thread (mmdevapi spawns it into this unix call),
 * so calling into ntdll here is legal — unlike from the RT callback. */
extern NTSTATUS NtSetEvent( HANDLE handle, void *prev_state );

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

/* ------------------- Tier-2: RemoteIO real output ------------------- */

/* Core Audio real-time thread. Ring + atomics ONLY — no Wine calls, no
 * locks, no allocation, no logging. Underrun = silence (WASAPI-correct:
 * padding drains to 0 and the position clock pauses at write_pos). */
static OSStatus ios_audio_render_cb(void *refcon, AudioUnitRenderActionFlags *flags,
                                    const AudioTimeStamp *ts, UInt32 bus,
                                    UInt32 nframes, AudioBufferList *iodata) {
    struct ios_stream *s = refcon;
    BYTE *out = (BYTE *)iodata->mBuffers[0].mData;
    UINT32 fb = s->frame_bytes;
    UINT32 cap = s->buffer_frames;
    uint64_t play = atomic_load_explicit(&s->play_pos, memory_order_relaxed);
    uint64_t wr = atomic_load_explicit(&s->write_pos, memory_order_acquire);
    uint64_t avail = wr - play;
    UInt32 tocopy = avail < nframes ? (UInt32)avail : nframes;
    UInt32 i = 0;
    (void)flags; (void)ts; (void)bus;
    while (i < tocopy) {
        UINT32 idx = (UINT32)((play + i) % cap);
        UINT32 chunk = cap - idx;
        if (chunk > tocopy - i) chunk = tocopy - i;
        memcpy(out + (size_t)i * fb, s->ring + (size_t)idx * fb, (size_t)chunk * fb);
        i += chunk;
    }
    if (tocopy < nframes)
        memset(out + (size_t)tocopy * fb, 0, (size_t)(nframes - tocopy) * fb);
    atomic_store_explicit(&s->play_pos, play + tocopy, memory_order_release);
    return noErr;
}

/* Parse the WASAPI format into "is float?" — tag 3 = IEEE float, tag
 * 0xFFFE = extensible (SubFormat GUID first byte: 1 PCM, 3 float). */
static int ios_fmt_is_float(const struct WAVEFORMATEX_stub *fmt) {
    if (!fmt) return 0;
    if (fmt->wFormatTag == 3) return 1;
    if (fmt->wFormatTag == 0xFFFE && fmt->cbSize >= 22) {
        const uint8_t *sub = (const uint8_t *)fmt + 24;
        return sub[0] == 3;
    }
    return 0;
}

/* Build the RemoteIO unit for the negotiated stream format. Returns 0 on
 * success; any failure leaves s->au NULL (null-mode fallback). */
static int ios_audio_setup_unit(struct ios_stream *s, const struct WAVEFORMATEX_stub *fmt) {
    AudioComponentDescription desc = {0};
    AudioComponent comp;
    AudioStreamBasicDescription asbd = {0};
    AURenderCallbackStruct cb;
    OSStatus err;

    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_RemoteIO;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) { fprintf(stderr, "[ios_audio] RemoteIO component not found\n"); return -1; }
    if ((err = AudioComponentInstanceNew(comp, &s->au))) {
        fprintf(stderr, "[ios_audio] AudioComponentInstanceNew: %d\n", (int)err);
        s->au = NULL; return -1;
    }

    asbd.mSampleRate = s->sample_rate;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsPacked |
        (ios_fmt_is_float(fmt) ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger);
    asbd.mBytesPerPacket = s->frame_bytes;
    asbd.mFramesPerPacket = 1;
    asbd.mBytesPerFrame = s->frame_bytes;
    asbd.mChannelsPerFrame = s->channels;
    asbd.mBitsPerChannel = (s->frame_bytes / s->channels) * 8;

    err = AudioUnitSetProperty(s->au, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Input, 0, &asbd, sizeof(asbd));
    if (err) {
        fprintf(stderr, "[ios_audio] SetProperty(StreamFormat rate=%u ch=%u fb=%u float=%d): %d\n",
                s->sample_rate, s->channels, s->frame_bytes, ios_fmt_is_float(fmt), (int)err);
        goto fail;
    }

    cb.inputProc = ios_audio_render_cb;
    cb.inputProcRefCon = s;
    err = AudioUnitSetProperty(s->au, kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input, 0, &cb, sizeof(cb));
    if (err) { fprintf(stderr, "[ios_audio] SetRenderCallback: %d\n", (int)err); goto fail; }

    if ((err = AudioUnitInitialize(s->au))) {
        fprintf(stderr, "[ios_audio] AudioUnitInitialize: %d\n", (int)err);
        goto fail;
    }
    fprintf(stderr, "[ios_audio] RemoteIO ready: %u Hz, %u ch, %u B/frame, float=%d, ring=%u frames\n",
            s->sample_rate, s->channels, s->frame_bytes, ios_fmt_is_float(fmt), s->buffer_frames);
    return 0;
fail:
    AudioComponentInstanceDispose(s->au);
    s->au = NULL;
    return -1;
}

static void ios_audio_teardown_unit(struct ios_stream *s) {
    if (!s->au) return;
    if (s->au_running) AudioOutputUnitStop(s->au);
    AudioUnitUninitialize(s->au);
    AudioComponentInstanceDispose(s->au);
    s->au = NULL;
    s->au_running = 0;
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
    /* CONTRACT (mmdevapi client.c main_loop_start): the PE side blocks
     * WaitForSingleObject(event, INFINITE) until the driver signals this
     * event. Returning WITHOUT signaling deadlocks whoever triggered
     * driver init — FMOD's IAudioClient path — which held Thumper on the
     * splash screen (2026-07-05; and likely the misread May "FMOD probes
     * then stops" observation). winecoreaudio does exactly this. */
    struct main_loop_params { HANDLE event; } *p = args;
    LOG_FN_CALL(2, "main_loop");
    NtSetEvent(p->event, NULL);
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
    uint64_t dur_frames;
    /* Allocate or reuse the singleton stream. */
    ios_audio_teardown_unit(&g_stream);
    g_stream.valid = 1;
    g_stream.started = 0;
    g_stream.start_mach = 0;
    g_stream.accumulated_frames = 0;
    g_stream.sample_rate = p->fmt && p->fmt->nSamplesPerSec ? p->fmt->nSamplesPerSec : IOS_AUDIO_SAMPLE_RATE;
    g_stream.channels = p->fmt && p->fmt->nChannels ? p->fmt->nChannels : IOS_AUDIO_CHANNELS;
    g_stream.frame_bytes = p->fmt && p->fmt->nBlockAlign ? p->fmt->nBlockAlign
                          : (g_stream.channels * IOS_AUDIO_BITS) / 8;
    /* Ring capacity: the requested buffer duration (100ns units), floor
     * 100ms so a slow FEX-translated mixer has slack. */
    dur_frames = (uint64_t)(p->duration > 0 ? p->duration : 0) * g_stream.sample_rate / 10000000ull;
    if (dur_frames < g_stream.sample_rate / 10) dur_frames = g_stream.sample_rate / 10;
    if (dur_frames > g_stream.sample_rate * 4) dur_frames = g_stream.sample_rate * 4;
    g_stream.buffer_frames = (UINT32)dur_frames;
    free(g_stream.ring);
    g_stream.ring = (BYTE *)calloc(g_stream.buffer_frames, g_stream.frame_bytes);
    free(g_stream.render_scratch);
    g_stream.scratch_frames = g_stream.buffer_frames;
    g_stream.render_scratch = (BYTE *)calloc(g_stream.scratch_frames, g_stream.frame_bytes);
    g_stream.pending_frames = 0;
    atomic_store(&g_stream.write_pos, 0);
    atomic_store(&g_stream.play_pos, 0);

    if (p->flow == eRender && g_stream.ring)
        ios_audio_setup_unit(&g_stream, p->fmt);   /* failure -> null-mode */

    if (p->channel_count) *p->channel_count = g_stream.channels;
    if (p->stream) *p->stream = (stream_handle)(uintptr_t)&g_stream;
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_release_stream(void *args) {
    struct release_stream_params *p = args;
    ios_audio_teardown_unit(&g_stream);
    g_stream.valid = 0;      /* timer_loop notices and exits */
    g_stream.started = 0;
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_start(void *args) {
    LOG_FN_CALL(6, "start");
    struct stream_handle_params *p = args;
    if (!g_stream.started) {
        if (g_stream.au && !g_stream.au_running) {
            OSStatus err = AudioOutputUnitStart(g_stream.au);
            if (err) {
                fprintf(stderr, "[ios_audio] AudioOutputUnitStart: %d — null-mode\n", (int)err);
                ios_audio_teardown_unit(&g_stream);
            } else {
                g_stream.au_running = 1;
            }
        }
        g_stream.start_mach = mach_absolute_time();
        g_stream.started = 1;
    }
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_stop(void *args) {
    struct stream_handle_params *p = args;
    if (g_stream.started) {
        if (g_stream.au && g_stream.au_running) {
            AudioOutputUnitStop(g_stream.au);
            g_stream.au_running = 0;
        }
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
    /* Drop queued-but-unplayed audio (only legal while stopped). */
    atomic_store(&g_stream.write_pos, 0);
    atomic_store(&g_stream.play_pos, 0);
    g_stream.pending_frames = 0;
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_timer_loop(void *args) {
    /* Runs on a dedicated Wine thread mmdevapi spawns for event-driven
     * clients. Wake the client every device period so it refills the
     * ring; exit when the stream dies. */
    (void)args;
    LOG_FN_CALL(9, "timer_loop");
    while (g_stream.valid) {
        usleep(10000); /* device period, 10 ms */
        if (g_stream.event && g_stream.started)
            NtSetEvent(g_stream.event, NULL);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS ios_get_render_buffer(void *args) {
    LOG_FN_CALL(10, "get_render_buffer");
    struct get_render_buffer_params *p = args;
    if (g_stream.au) {
        uint64_t padding = atomic_load(&g_stream.write_pos) - atomic_load(&g_stream.play_pos);
        if (p->frames + padding > g_stream.buffer_frames) {
            p->result = (HRESULT)0x88890006L; /* AUDCLNT_E_BUFFER_TOO_LARGE */
            if (p->data) *p->data = NULL;
            return STATUS_SUCCESS;
        }
    }
    if (p->frames > g_stream.scratch_frames) {
        /* Client asked for more than the ring — grow scratch; the copy in
         * release clamps to ring capacity anyway. */
        BYTE *ns = (BYTE *)realloc(g_stream.render_scratch,
                                   (size_t)p->frames * g_stream.frame_bytes);
        if (!ns) { p->result = E_FAIL; return STATUS_SUCCESS; }
        g_stream.render_scratch = ns;
        g_stream.scratch_frames = p->frames;
    }
    g_stream.pending_frames = p->frames;
    if (p->data) *p->data = g_stream.render_scratch;
    p->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_release_render_buffer(void *args) {
    struct release_render_buffer_params *p = args;
    if (g_stream.au && p->written_frames > 0) {
        UINT32 fb = g_stream.frame_bytes;
        UINT32 cap = g_stream.buffer_frames;
        UINT32 n = p->written_frames;
        uint64_t wr = atomic_load_explicit(&g_stream.write_pos, memory_order_relaxed);
        UINT32 i = 0;
        if (n > g_stream.pending_frames) n = g_stream.pending_frames;
        if (p->flags & 0x2 /* AUDCLNT_BUFFERFLAGS_SILENT */)
            memset(g_stream.render_scratch, 0, (size_t)n * fb);
        while (i < n) {
            UINT32 idx = (UINT32)((wr + i) % cap);
            UINT32 chunk = cap - idx;
            if (chunk > n - i) chunk = n - i;
            memcpy(g_stream.ring + (size_t)idx * fb,
                   g_stream.render_scratch + (size_t)i * fb, (size_t)chunk * fb);
            i += chunk;
        }
        /* release-store AFTER the copy so the RT callback never reads
         * frames that aren't fully written */
        atomic_store_explicit(&g_stream.write_pos, wr + n, memory_order_release);
    }
    g_stream.pending_frames = 0;
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
    LOG_FN_CALL(14, "is_format_supported");
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
    LOG_FN_CALL(16, "get_mix_format");
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
    if (p->frames) *p->frames = g_stream.buffer_frames ? g_stream.buffer_frames
                                                       : IOS_AUDIO_BUFFER_FRAMES;
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
    if (p->padding) {
        if (g_stream.au) {
            uint64_t pad = atomic_load(&g_stream.write_pos) - atomic_load(&g_stream.play_pos);
            *p->padding = (UINT32)(pad > g_stream.buffer_frames ? g_stream.buffer_frames : pad);
        } else {
            *p->padding = 0; /* null-mode: always hungry */
        }
    }
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
    /* THIS is the function that drives FMOD's clock. Tier-2: frames the
     * RT callback actually consumed — the true hardware clock. Null-mode
     * fallback: wall-clock synthesis as before. */
    if (p->pos) {
        if (g_stream.au)
            *p->pos = atomic_load(&g_stream.play_pos);
        else
            *p->pos = elapsed_frames(&g_stream);
    }
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
