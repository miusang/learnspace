

#include <SDL2/SDL.h>
#include <SDL2/SDL_events.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/fifo.h>
#include <libavutil/log.h>
#include <libavutil/macros.h>
#include <libavutil/time.h>

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

#define PACKET_QUEUE_NB 32

#define REFRESH_RATE 0.01

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1

static SDL_Window *window;
static SDL_Renderer *renderer;

static int64_t audio_callback_time;

static const struct TextureFormatEntry {
    enum AVPixelFormat format;
    int texture_fmt;
} sdl_texture_format_map[] = {
    { AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332 },
    { AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_RGB444 },
    { AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_RGB555 },
    { AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_BGR555 },
    { AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565 },
    { AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565 },
    { AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24 },
    { AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24 },
    { AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_RGB888 },
    { AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_BGR888 },
    { AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888 },
    { AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888 },
    { AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888 },
    { AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888 },
    { AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888 },
    { AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888 },
    { AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV },
    { AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2 },
    { AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY },
    { AV_PIX_FMT_NONE,           SDL_PIXELFORMAT_UNKNOWN },
};

typedef struct AudioParams {
    int freq;
    int channels;
    int frame_size;
    int bytes_per_sec;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
} AudioParams;

typedef struct Clock {
    double pts;
    double pts_drift;
    double last_updated;
} Clock;

typedef struct Frame {
    AVFrame *frame;
    double pts;
    double duration;
} Frame;

typedef struct FrameQueue {
    Frame queue[FRAME_QUEUE_SIZE];
    int rindex;
    int windex;
    int size;
    int max_size;
    int keep_last;
    int rindex_shown;
    SDL_cond *cond;
    SDL_mutex *mutex;
} FrameQueue;

typedef struct PacketQueue {
    AVFifoBuffer *pkt_list;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

typedef struct Decoder {
    AVCodecContext *avctx;
    SDL_Thread *decoder_tid;
    int finished;
} Decoder;

typedef struct VideoState {
    AVFormatContext *ic;
    Decoder auddec;
    Decoder viddec;
    
    Clock vidclk;
    Clock audclk;

    PacketQueue audioq;
    PacketQueue videoq;

    FrameQueue pictq;
    FrameQueue sampq;

    AVStream *video_st;
    SDL_Texture *vid_texture;
    SDL_Thread *read_tid;

    struct AudioParams audio_src;
    struct SwsContext *sws_ctx;
    struct SwrContext *swr_ctx;

    double max_frame_duration;
    double frame_timer; // 帧的显示时刻
    double audio_clock; // 音频播放时刻（一帧播放完的时刻）

    uint8_t *audio_buf;
    int audio_buf_size; // buf总大小
    int audio_buf_index; // 未使用的头部索引
    int audio_hw_buf_size; // sdl驱动中未播放的buffer大小
    int eof;
    int abort_request;
    int video_stream;
    int audio_stream;
    const char *file_name;

} VideoState;

void print_error(const char *filename, int err) {
    char errbuf[128];
    const char *errbuf_ptr = errbuf;

    if (av_strerror(err, errbuf, sizeof(errbuf)) < 0)
        errbuf_ptr = strerror(AVUNERROR(err));
    av_log(NULL, AV_LOG_ERROR, "%s: %s\n", filename, errbuf_ptr);
}

static void get_sdl_pix_fmt_and_blendmode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode)
{
    int i;
    *sdl_blendmode = SDL_BLENDMODE_NONE;
    *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
    if (format == AV_PIX_FMT_RGB32   ||
        format == AV_PIX_FMT_RGB32_1 ||
        format == AV_PIX_FMT_BGR32   ||
        format == AV_PIX_FMT_BGR32_1)
        *sdl_blendmode = SDL_BLENDMODE_BLEND;
    for (i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++) {
        if (format == sdl_texture_format_map[i].format) {
            *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
            return;
        }
    }
}

static void set_clock_at(Clock *c, double pts, double time) {
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = pts - time;
}

static void set_clock(Clock *c, double pts) {
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, time);
}

static double get_clock(Clock *c) {
    double time = av_gettime_relative() / 1000000.0;
    return c->pts_drift + time;
}

static double get_master_clock(VideoState *is) {
    // 此处同步时钟默认是音频时钟，即同步到音频
    return get_clock(&is->audclk);
}

static int frame_queue_init(FrameQueue *q, int max_size, int keep_last) {
    int i;
    memset(q, 0, sizeof(FrameQueue));
    q->mutex = SDL_CreateMutex();
    if (!q->mutex) {
        av_log(NULL, AV_LOG_ERROR, "Sdl create mutex failed, %s.\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->cond = SDL_CreateCond();
    if (!q->cond) {
        av_log(NULL, AV_LOG_ERROR, "Sdl create cond failed, %s.\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    q->keep_last = !!keep_last;
    for (i = 0; i < max_size; i++) {
        if (!(q->queue[i].frame = av_frame_alloc())) {
            av_log(NULL, AV_LOG_ERROR, "av_frame_alloc failed.\n");
            return AVERROR(ENOMEM);
        }
    }
    return 0;
}

static Frame* frame_queue_peek(FrameQueue *q) {
    return &q->queue[(q->rindex + q->rindex_shown) % q->max_size];
}

static Frame* frame_queue_peek_last(FrameQueue *q) {
    return &q->queue[q->rindex];
}

static Frame* frame_queue_peek_writable(FrameQueue *q) {
    SDL_LockMutex(q->mutex);
    while (q->size >= q->max_size) {
        SDL_CondWait(q->cond, q->mutex);
    }
    SDL_UnlockMutex(q->mutex);
    return &q->queue[q->windex];
}

static Frame* frame_queue_peek_readable(FrameQueue *q) {
    SDL_LockMutex(q->mutex);
    while (q->size <= 0) {
        SDL_CondWait(q->cond, q->mutex);
    }
    SDL_UnlockMutex(q->mutex);
    return &q->queue[(q->rindex + q->rindex_shown) % q->max_size];
}

// 需要先用peek_writable
static void frame_queue_push(FrameQueue *q) {
    if (++q->windex >= q->max_size) {
        q->windex = 0;
    }
    SDL_LockMutex(q->mutex);
    q->size++;
    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
}

static void frame_queue_pop(FrameQueue *q) {
    if (q->keep_last && !q->rindex_shown) {
        q->rindex_shown = 1;
        return;
    }
    av_frame_unref(q->queue[q->rindex].frame);
    if (++q->rindex >= q->max_size) {
        q->rindex = 0;
    }
    SDL_LockMutex(q->mutex);
    q->size--;
    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
}

static int frame_queue_nb_remaining(FrameQueue *q) {
    return q->size - q->rindex_shown;
}

static int packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->pkt_list = av_fifo_alloc_array(PACKET_QUEUE_NB, sizeof(AVPacket *));
    if (!q->pkt_list) {
        return AVERROR(ENOMEM);
    }
    q->mutex = SDL_CreateMutex();
    if (!q->mutex) {
        av_log(NULL, AV_LOG_ERROR, "Sdl create mutex failed, %s.\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->cond = SDL_CreateCond();
    if (!q->cond) {
        av_log(NULL, AV_LOG_ERROR, "Sdl create cond failed, %s.\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    AVPacket *pkt1 = av_packet_alloc();
    SDL_LockMutex(q->mutex);
    av_packet_move_ref(pkt1, pkt);
    av_fifo_generic_write(q->pkt_list, &pkt1, sizeof(AVPacket *), NULL);
    q->nb_packets++;
    q->size += pkt->size;
    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
    return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt) {
    AVPacket *pkt1 = av_packet_alloc();
    SDL_LockMutex(q->mutex);
    if (av_fifo_size(q->pkt_list) <= 0) {
        SDL_CondWait(q->cond, q->mutex);
    }
    av_fifo_generic_read(q->pkt_list, &pkt1, sizeof(AVPacket *), NULL);
    av_packet_move_ref(pkt, pkt1);
    av_packet_free(&pkt1);
    q->nb_packets--;
    q->size -= pkt->size;
    SDL_UnlockMutex(q->mutex);
    return 0;
}

static void queue_picture(FrameQueue *q, AVFrame *frame, double pts, double duration) {
    Frame *vf;
    vf = frame_queue_peek_writable(q);
    vf->pts = pts;
    vf->duration = duration;
    av_frame_move_ref(vf->frame, frame);
    frame_queue_push(q);
}

int audio_decode_frame(VideoState *is) {
    Frame *af;
    static AVFrame *frame = NULL;
    static uint8_t *resample_buf = NULL;
    static int resample_buf_len;
    static int resample_size;

    if (!frame) {
        frame = av_frame_alloc();
    }
    af = frame_queue_peek_readable(&is->sampq);
    frame = af->frame;
    frame_queue_pop(&is->sampq);
    
    if (!is->swr_ctx && (frame->format != is->audio_src.fmt ||
                         frame->channel_layout != is->audio_src.channel_layout ||
                         frame->sample_rate != is->audio_src.freq)) {
         is->swr_ctx = swr_alloc_set_opts(NULL, is->audio_src.channel_layout, is->audio_src.fmt, 
         is->audio_src.freq, frame->channel_layout, frame->format, frame->sample_rate, 0, NULL);
        if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Create sample rate converter failed.\n");
            return -1;
        }
    }
        
    if (is->swr_ctx) { // 需要进行重采样
        const uint8_t **in = (const uint8_t**) frame->extended_data;
        uint8_t **out = &resample_buf;
         int nb_resamples;
        // 此处为啥多加256？
        int out_count = (int64_t)frame->nb_samples * is->audio_src.freq / frame->sample_rate + 256;
        int out_size = av_samples_get_buffer_size(NULL, is->audio_src.channels, out_count, is->audio_src.fmt, 0);
        if (resample_buf == NULL) {
            av_fast_malloc(&resample_buf, &resample_buf_len, out_size);
        }
        nb_resamples = swr_convert(is->swr_ctx, out, out_count, in, frame->nb_samples);
        is->audio_buf = resample_buf;
        resample_size = nb_resamples * is->audio_src.channels * av_get_bytes_per_sample(is->audio_src.fmt);
    } else { // 不需要进行重采样
        av_log(NULL, AV_LOG_DEBUG, "no resample.\n");
        is->audio_buf = frame->data[0];
        resample_size = av_samples_get_buffer_size(NULL, is->auddec.avctx->channels, frame->nb_samples, is->auddec.avctx->sample_fmt, 1);
    }
    if (!isnan(af->pts)) {
        is->audio_clock = af->pts + (double) af->frame->nb_samples / af->frame->sample_rate;
    } else {
        is->audio_clock = NAN;
    }
    return resample_size;
}

static void sdl_audio_callback(void *opaque, uint8_t*stream, int len) {
    int len1;
    double rest_time;
    VideoState *is = (VideoState*) opaque;
    audio_callback_time = av_gettime_relative();
    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
            is->audio_buf_size = audio_decode_frame(is);
            if (is->audio_buf <= 0) {
                is->audio_buf = NULL;
                is->audio_buf_size = 1024;
            }
            is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len) {
            len1 = len;
        }
        if (is->audio_buf) {
            memcpy(stream, is->audio_buf + is->audio_buf_index, len1);
        } else {
            memset(stream, 0, len1);
        }
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    // 更新audio时钟
    if (!isnan(is->audio_clock)) {
        // av_log(NULL, AV_LOG_DEBUG, "hw buffer size %d.\n", is->audio_hw_buf_size);
        // 乘2，是sdl驱动采用双buffer机制
        rest_time = (double)(2 * is->audio_hw_buf_size + is->audio_buf_size - is->audio_buf_index) / is->audio_src.bytes_per_sec;
        set_clock_at(&is->audclk, is->audio_clock - rest_time, audio_callback_time / 1000000.0);
    }
}

static int audio_open(VideoState *is) {
    SDL_AudioSpec wanted_spec, spec;
    SDL_AudioDeviceID audio_dev;

    wanted_spec.channels = is->auddec.avctx->channels; // 声道数
    wanted_spec.freq = is->auddec.avctx->sample_rate; // 采样率
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }
    wanted_spec.format = AUDIO_S16SYS;  // 数据格式
    wanted_spec.silence = 0;            // 静音值
    wanted_spec.samples = 1024;
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = is;
    
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!audio_dev) {
        av_log(NULL, AV_LOG_ERROR, "SDL_OpenAudioDevice failed, %s\n", SDL_GetError());
        return -1;
    }
    
    is->audio_src.fmt = AV_SAMPLE_FMT_S16;
    is->audio_src.freq = spec.freq;
    av_log(NULL, AV_LOG_DEBUG, "sample rate %d\n", spec.freq);
    is->audio_src.channel_layout = av_get_default_channel_layout(spec.channels);
    is->audio_src.channels = spec.channels;
    is->audio_src.frame_size = av_samples_get_buffer_size(NULL, spec.channels, 1, is->audio_src.fmt, 1);
    is->audio_src.bytes_per_sec = av_samples_get_buffer_size(NULL, spec.channels, is->audio_src.freq, is->audio_src.fmt, 1);
    if (is->audio_src.bytes_per_sec <= 0 || is->audio_src.frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed.\n");
        return -1;
    }

    is->audio_hw_buf_size = spec.size;
    SDL_PauseAudioDevice(audio_dev, 0);
    return 0;
}

static int audio_thread(void *arg) {
    int ret;
    Frame *af;
    AVRational tb;
    VideoState *is = (VideoState*) arg;
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();

    audio_open(is);

    for (;;) {
        if (is->eof && (is->audioq.nb_packets <= 0)) {
            is->auddec.finished = 1;
            break;
        }
        packet_queue_get(&is->audioq, pkt);    
        if (!pkt) {
            av_log(NULL, AV_LOG_ERROR, "video packet get error.\n");
            break;
        }
        ret = avcodec_send_packet(is->auddec.avctx, pkt);
        if (ret < 0) {
            print_error(is->file_name, ret);
            break;
        }
        av_packet_unref(pkt);
        if (ret < 0) {
            print_error(is->file_name, ret);
            break;
        }

        ret = avcodec_receive_frame(is->auddec.avctx, frame);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                continue;
            }
            print_error(is->file_name, ret);
            break;
        }
        af = frame_queue_peek_writable(&is->sampq);
        tb = (AVRational){1, frame->sample_rate};
        af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
        af->duration = av_q2d((AVRational){frame->nb_samples, frame->sample_rate});
        av_frame_move_ref(af->frame, frame);
        frame_queue_push(&is->sampq);
    }
    av_log(NULL, AV_LOG_ERROR, "audio thread quit.\n");
    return 0;
}

static int video_thread(void *arg) {
    int ret;
    double pts, duration;
    VideoState *is = (VideoState*)arg;
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    AVRational tb = is->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

    if (!is) {
        av_log(NULL, AV_LOG_ERROR, "user data is NULL.\n");
        return -1;
    }
    for (;;) {
        if (is->eof && (is->videoq.nb_packets <= 0)) {
            is->viddec.finished = 1;
            break;
        }
        packet_queue_get(&is->videoq, pkt);    
        if (!pkt) {
            av_log(NULL, AV_LOG_ERROR, "video packet get error.\n");
            break;
        }
        ret = avcodec_send_packet(is->viddec.avctx, pkt);
        if (ret < 0) {
            print_error(is->file_name, ret);
            break;
        }
        av_packet_unref(pkt);
        if (ret < 0) {
            print_error(is->file_name, ret);
            break;
        }

        ret = avcodec_receive_frame(is->viddec.avctx, frame);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                continue;
            }
            print_error(is->file_name, ret);
            break;
        }
        pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
        duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
        queue_picture(&is->pictq, frame, pts, duration);
    }
    av_log(NULL, AV_LOG_ERROR, "video thread quit.\n");
    return 0;
}

static int stream_component_open(VideoState *is, int stream_index) {
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    const AVCodec *codec;
    AVRational rational;
    int ret;

    avctx = avcodec_alloc_context3(NULL);
    if (!avctx) {
        av_log(NULL, AV_LOG_ERROR, "alloc avcodec context failed.\n");
        return AVERROR(ENOMEM);
    }
    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0) {
        goto fail;
    }
    codec = avcodec_find_decoder(avctx->codec_id);
    if (!codec) {
        goto fail;
    }
    ret = avcodec_open2(avctx, codec, NULL);
    if (ret < 0) {
        goto fail;
    }

    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->auddec.avctx = avctx;
        is->auddec.decoder_tid = SDL_CreateThread(audio_thread, "audio_decoder", is);
        if (!is->auddec.decoder_tid) {
            av_log(NULL, AV_LOG_ERROR, "create audio decoder thread failed, %s.\n", SDL_GetError());
            goto fail;
        }
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->viddec.avctx = avctx;
        is->video_st = is->ic->streams[stream_index];
        SDL_SetWindowSize(window, avctx->width, avctx->height);
        is->viddec.decoder_tid = SDL_CreateThread(video_thread, "video_decoder", is);
        if (!is->viddec.decoder_tid) {
            av_log(NULL, AV_LOG_ERROR, "create video decoder thread failed, %s.\n", SDL_GetError());
            goto fail;
        }
        break;
    default:
        break;
    }
    return 0;
fail:
    avcodec_free_context(&avctx);
    return -1;
}

static int read_thread(void *arg) {
    VideoState *is = (VideoState *)arg;
    SDL_Event event;
    int err;
    AVPacket *pkt = NULL;

    err = avformat_open_input(&is->ic, is->file_name, NULL, NULL);
    if (err < 0) {
        print_error(is->file_name, err);
        goto fail;
    }

    is->max_frame_duration = (is->ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    err = avformat_find_stream_info(is->ic, NULL);
    if (err < 0) {
        print_error(is->file_name, err);
        goto fail;
    }

    av_dump_format(is->ic, 0, is->file_name, 0);

    is->video_stream = av_find_best_stream(is->ic, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (is->video_stream < 0) {
        av_log(NULL, AV_LOG_ERROR, "could not find video stream.\n");
        goto fail;
    }
    is->audio_stream = av_find_best_stream(is->ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (is->audio_stream  < 0) {
        av_log(NULL, AV_LOG_ERROR, "could not find audio stream.\n");
        goto fail;
    }
    if (stream_component_open(is, is->video_stream) < 0) {
        goto fail;
    }
    if (stream_component_open(is, is->audio_stream) < 0) {
        goto fail;
    }
    pkt = av_packet_alloc();
    for (;;) {
        if (is->videoq.nb_packets >= PACKET_QUEUE_NB || is->audioq.nb_packets >= PACKET_QUEUE_NB) {
            // av_log(NULL, AV_LOG_DEBUG, "packet buffer is enough, wait 10ms. v_nb %d, a_nb %d\n", is->videoq.nb_packets, is->audioq.nb_packets);
            // wait 10ms
            av_usleep(10 * 1000);
            continue;
        }
        err = av_read_frame(is->ic, pkt);
        if (err < 0) {
            if (err == AVERROR(EAGAIN)) {
                continue;
            } else if (err == AVERROR_EOF) {
                is->eof = 1;
                err = 0;
                break;
            }
            print_error(is->file_name, err);
            break;
        }
        if (pkt->stream_index == is->video_stream) {
            packet_queue_put(&is->videoq, pkt);
        } else if (pkt->stream_index == is->audio_stream) {
            packet_queue_put(&is->audioq, pkt);
        }
    }
        
fail:
    av_packet_free(&pkt);
    if (err < 0) {   
        event.type = SDL_QUIT;
        SDL_PushEvent(&event);
    }
    av_log(NULL, AV_LOG_ERROR, "read thread quit.\n");
    return 0;
}

VideoState *stream_open(const char *input_file) {
     int err;
    VideoState *is = (VideoState*) av_mallocz(sizeof(VideoState));
    if (!is) {
        return NULL;
    }
    if (packet_queue_init(&is->videoq) < 0 || 
        packet_queue_init(&is->audioq) < 0 ||
        frame_queue_init(&is->pictq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0 ||
        frame_queue_init(&is->sampq, SAMPLE_QUEUE_SIZE, 1) < 0) {
        av_log(NULL, AV_LOG_ERROR, "queue init failed.\n");
        goto fail;
    }
    
    is->file_name = input_file;
    is->read_tid = SDL_CreateThread(read_thread, "read_thread", is);
    if (!is->read_tid) {
        goto fail;
    }
    return is;
fail:
    av_free(is);
    return NULL;
}

static int realloc_texture(SDL_Texture **texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode, int init_texture)
{
    Uint32 format;
    int access, w, h;
    if (!*texture || SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h || new_format != format) {
        void *pixels;
        int pitch;
        if (*texture)
            SDL_DestroyTexture(*texture);
        if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
            return -1;
        if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
            return -1;
        if (init_texture) {
            if (SDL_LockTexture(*texture, NULL, &pixels, &pitch) < 0)
                return -1;
            memset(pixels, 0, pitch * new_height);
            SDL_UnlockTexture(*texture);
        }
        av_log(NULL, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", new_width, new_height, SDL_GetPixelFormatName(new_format));
    }
    return 0;
}

static int upload_texture(SDL_Texture **tex, AVFrame *frame, struct SwsContext **img_convert_ctx) {
    int ret = 0;
    Uint32 sdl_pix_fmt;
    SDL_BlendMode sdl_blendmode;
    get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
    if (realloc_texture(tex, sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt, frame->width, frame->height, sdl_blendmode, 0) < 0)
        return -1;
    switch (sdl_pix_fmt) {
        case SDL_PIXELFORMAT_UNKNOWN:
            /* This should only happen if we are not using avfilter... */
            *img_convert_ctx = sws_getCachedContext(*img_convert_ctx,
                frame->width, frame->height, frame->format, frame->width, frame->height,
                AV_PIX_FMT_BGRA, SWS_BICUBIC, NULL, NULL, NULL);
            if (*img_convert_ctx != NULL) {
                uint8_t *pixels[4];
                int pitch[4];
                if (!SDL_LockTexture(*tex, NULL, (void **)pixels, pitch)) {
                    sws_scale(*img_convert_ctx, (const uint8_t * const *)frame->data, frame->linesize,
                              0, frame->height, pixels, pitch);
                    SDL_UnlockTexture(*tex);
                }
            } else {
                av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                ret = -1;
            }
            break;
        case SDL_PIXELFORMAT_IYUV:
            if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {
                ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0], frame->linesize[0],
                                                       frame->data[1], frame->linesize[1],
                                                       frame->data[2], frame->linesize[2]);
            } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {
                ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height                    - 1), -frame->linesize[0],
                                                       frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[1],
                                                       frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[2]);
            } else {
                av_log(NULL, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
                return -1;
            }
            break;
        default:
            if (frame->linesize[0] < 0) {
                ret = SDL_UpdateTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]);
            } else {
                ret = SDL_UpdateTexture(*tex, NULL, frame->data[0], frame->linesize[0]);
            }
            break;
    }
    return ret;
}
static void video_display(VideoState *is) {
    Frame *vf;
    SDL_Rect rect;
    double time;
    time = av_gettime_relative() / 1000000.0;
    // av_log(NULL, AV_LOG_ERROR, "display time %3f.\n", time);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    
    vf = frame_queue_peek_last(&is->pictq);
    rect.x = 0;
    rect.y = 0;
    rect.w = vf->frame->width;
    rect.h = vf->frame->height;
    upload_texture(&is->vid_texture, vf->frame, &is->sws_ctx);
    SDL_RenderCopyEx(renderer, is->vid_texture, NULL, &rect, 0, NULL, 0);
    SDL_RenderPresent(renderer);
}

// 计算经过va同步后，当前帧应该延迟delay时间后进行显示
static double compute_target_delay(VideoState *is, double delay) {
    double diff, sync_treshold;
    diff = get_clock(&is->vidclk) - get_master_clock(is);
    sync_treshold = FFMIN(AV_SYNC_THRESHOLD_MIN, FFMAX(AV_SYNC_THRESHOLD_MAX, delay));
    if (!isnan(diff) && fabs(diff) < is->max_frame_duration) {
        if (diff <= - sync_treshold) { // 落后同步时钟，超过阈值
            diff = FFMAX(0, delay + diff); // 要进行追赶
        } else if (diff >= sync_treshold && diff > AV_SYNC_FRAMEDUP_THRESHOLD) {
            delay += diff; // 超前同步时钟，超过阈值，第1种放慢
        } else if (diff >= sync_treshold) {
            delay *= 2; // 超前同步时钟，超过阈值，第2种放慢
        }
    }
    return delay;
}

// 计算当前帧理论duration
static double vf_duration(VideoState *is, Frame *cur, Frame *next) {
    double duration = next->pts - cur->pts; // 单位秒
    if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration) {
        duration = cur->duration;
    }
    return duration;
}

static void update_video_pts(Clock *c, double pts) {
    set_clock(c, pts);
}

static void video_refresh(VideoState *is, double *remaining_time) {
    Frame *vf, *last_vf;
    double last_duration, delay, time;

    if (frame_queue_nb_remaining(&is->pictq) <= 0) {
        return;
    }
    last_vf = frame_queue_peek_last(&is->pictq);
    vf = frame_queue_peek(&is->pictq);
    last_duration = vf_duration(is, last_vf, vf);
    // 当前帧要延迟delay时间，再进行显示
    delay = compute_target_delay(is, last_duration);

    time = av_gettime_relative() / 1000000.0;
    if (time < is->frame_timer + delay) { // 当前时刻小于，当前帧显示时刻（上一帧显示时刻 + delay)
        *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time); // 矫正上一帧延迟时间
        // av_log(NULL, AV_LOG_ERROR, "remaining time %3f.\n", *remaining_time);
        goto display;                     // 表明当前帧的显示时刻还没到，继续显示上一帧
    }

    is->frame_timer += delay; // 更新显示时刻
    if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX) {
        is->frame_timer = time; // 校准显示时刻
    }
    SDL_LockMutex(is->pictq.mutex);
    if (!isnan(vf->pts)) {
        update_video_pts(&is->vidclk, vf->pts); // 更新video时钟
    }
    SDL_UnlockMutex(is->pictq.mutex);
    frame_queue_pop(&is->pictq); // 这里删除上一帧
display:
    video_display(is);
}

static void refresh_loop_wait_event(VideoState *is, SDL_Event *event) {
    double remaining_time = 0.0;
    SDL_PumpEvents();
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
        if (remaining_time > 0.0)
            av_usleep((int64_t)(remaining_time * 1000000.0));
        remaining_time = REFRESH_RATE;
        video_refresh(is, &remaining_time);
        SDL_PumpEvents();
    }
}

void do_exit() {
    exit(0);
}

void event_loop(VideoState *is) {
    SDL_Event event;
    for (;;) {
        refresh_loop_wait_event(is, &event);
        switch (event.type) {
        case SDL_QUIT:
            do_exit();
            break;
        case SDL_KEYDOWN:     
            switch (event.key.keysym.sym) {
            case SDLK_q:
                do_exit();
                break;    
            default:
                break;
            }
            break;
        default:
            break;
        }
    }
}

int main(int argc, char **argv) {
 VideoState *is = NULL;
    int err;

    av_log_set_level(AV_LOG_DEBUG);
    if (argc < 2) {
        av_log(NULL, AV_LOG_ERROR, "Input file not specified.\n");
        return -1;
    }
    err = SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_TIMER); 
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Sdl init failed, %s.\n", SDL_GetError());
        return -1;
    }
    window = SDL_CreateWindow("test", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                640, 480, SDL_WINDOW_RESIZABLE | SDL_WINDOW_RESIZABLE);
    if (!window) {
        av_log(NULL, AV_LOG_ERROR, "create window failed, %s.\n", SDL_GetError());
        return -1;
    }
    renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) {
        av_log(NULL, AV_LOG_ERROR, "create render failed, %s\n.", SDL_GetError());
        return -1;
    }
    is = stream_open(argv[1]);
    if (!is) {
        av_log(NULL, AV_LOG_ERROR, "open stream failed.\n");
        return -1;
    }

    event_loop(is);

    return 0;
    return 0;
}