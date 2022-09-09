/*
 * test video and audio without sync.
 */

#include <SDL2/SDL.h>
#include <SDL2/SDL_events.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/fifo.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#define MAX_PACKET_QUEUE_NB 16
#define MAX_FRAME_QUEUE_NB 16
#define MAX_PICTURE_QUEUE_NB 3


#define VIDEO_REFRESH_EVENT     SDL_USEREVENT + 1


typedef struct AudioParams {
    int freq;
    int channels;
    int frame_size;
    int bytes_per_sec;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
} AudioParams;

typedef struct PictureQueue {
    AVFrame *frame[MAX_PICTURE_QUEUE_NB];
    int nb_pictures;
    int windex;
    int rindex;
} PictureQueue;


typedef struct PacketQueue {
    AVFifoBuffer *pkt_list;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

typedef struct FrameQueue {
    AVFifoBuffer *queue;
    int nb_frames;
    SDL_mutex *mutex;
    SDL_cond *cond;
} FrameQueue;

typedef struct VideoState {
    AVFormatContext *ic;
    AVCodecContext *vid_ctx;
    AVCodecContext *aud_ctx;
    SDL_Texture *vid_texture;
    PacketQueue audioq;
    PacketQueue videoq;
    FrameQueue aud_frame_q;
    FrameQueue vid_frame_q;
    SDL_Thread *read_tid;
    SDL_Thread *audio_tid;
    SDL_Thread *video_tid;
    SDL_cond *continue_read_thread;
    // PictureQueue pictq;
    AVFrame *yuv_frame;
    struct AudioParams audio_src;
    struct SwsContext *sws_ctx;
    struct SwrContext *swr_ctx;

    uint8_t *audio_buf;
    int audio_buf_size; // buf�ܴ�С
    int audio_buf_index; // δʹ�õ�ͷ������
    int interval;
    int eof;
    int abort_request;
    int video_stream;
    int audio_stream;
    const char *file_name;
} VideoState;

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;


void print_error(const char *filename, int err) {
    char errbuf[128];
    const char *errbuf_ptr = errbuf;

    if (av_strerror(err, errbuf, sizeof(errbuf)) < 0)
        errbuf_ptr = strerror(AVUNERROR(err));
    av_log(NULL, AV_LOG_ERROR, "%s: %s\n", filename, errbuf_ptr);
}

void avfifo_test() {
    AVFifoBuffer *buffer = av_fifo_alloc_array(10, sizeof(int));
    av_log(NULL, AV_LOG_ERROR, "space %d, size %d.\n", av_fifo_space(buffer),  av_fifo_size(buffer));
    int a = 1, b = 2, c = 3;
    av_fifo_generic_write(buffer, &a, sizeof(int), NULL);
    av_fifo_generic_write(buffer, &b, sizeof(int), NULL);
    av_fifo_generic_write(buffer, &c, sizeof(int), NULL);
    av_log(NULL, AV_LOG_ERROR, "space %d, size %d.\n", av_fifo_space(buffer),  av_fifo_size(buffer));
    a = 6;
    int d, e, f;
    // av_fifo_generic_peek(buffer, &d, sizeof(int), NULL);
    av_fifo_generic_read(buffer, &d, sizeof(int), NULL);
    // av_fifo_drain(buffer, sizeof(int));
    av_fifo_generic_read(buffer, &e, sizeof(int), NULL);
    // av_fifo_drain(buffer, sizeof(int));
    av_fifo_generic_read(buffer, &f, sizeof(int), NULL);
    av_log(NULL, AV_LOG_ERROR, "d e f: %d , %d, %d. size %d\n", d, e, f, av_fifo_size(buffer));
}

static int packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->pkt_list = av_fifo_alloc_array(MAX_PACKET_QUEUE_NB, sizeof(AVPacket *));
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
    // if (av_fifo_space(q->pkt_list) <= 0) {
    //     return -1;
    // }
    AVPacket *pkt1 = av_packet_alloc();
    // av_log(NULL, AV_LOG_ERROR, "size %d, size1 %d.\n", pkt->size, pkt1->size);
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

static int frame_queue_init(FrameQueue *q) {
    memset(q, 0, sizeof(FrameQueue));
    q->queue = av_fifo_alloc_array(MAX_FRAME_QUEUE_NB, sizeof(AVFrame *));
    if (!q->queue) {
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
static int frame_queue_put(FrameQueue *q, AVFrame *frame) {
    AVFrame *frame1 = av_frame_alloc();
    SDL_LockMutex(q->mutex);
    av_frame_move_ref(frame1, frame);
    av_fifo_generic_write(q->queue, &frame1, sizeof(AVFrame *), NULL);
    q->nb_frames++;
    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
    return 0;
}
static int frame_queue_get(FrameQueue *q, AVFrame *frame) {
    AVFrame *frame1 = av_frame_alloc();
    SDL_LockMutex(q->mutex);
    if (av_fifo_size(q->queue) <= 0) {
        SDL_CondWait(q->cond, q->mutex);
    }
    av_fifo_generic_read(q->queue, &frame1, sizeof(AVFrame *), NULL);
    av_frame_move_ref(frame, frame1);
    av_frame_free(&frame1);
    q->nb_frames--;
    SDL_UnlockMutex(q->mutex);
    return 0;
}

int audio_decode_frame(VideoState *is) {
    static AVFrame *frame = NULL;
    static uint8_t *resample_buf = NULL;
    static int resample_buf_len;
    static int resample_size;

    if (!frame) {
        frame = av_frame_alloc();
    }
    frame_queue_get(&is->aud_frame_q, frame);
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
    if (is->swr_ctx) { // ��Ҫ�����ز���
        const uint8_t **in = (const uint8_t**) frame->extended_data;
        uint8_t **out = &resample_buf;
         int nb_resamples;
        // �˴�Ϊɶ���256��
        int out_count = (int64_t)frame->nb_samples * is->audio_src.freq / frame->sample_rate + 256;
        int out_size = av_samples_get_buffer_size(NULL, is->audio_src.channels, out_count, is->audio_src.fmt, 0);
        if (resample_buf == NULL) {
            av_fast_malloc(&resample_buf, &resample_buf_len, out_size);
        }
        nb_resamples = swr_convert(is->swr_ctx, out, out_count, in, frame->nb_samples);
        is->audio_buf = resample_buf;
        resample_size = nb_resamples * is->audio_src.channels * av_get_bytes_per_sample(is->audio_src.fmt);
    } else { // ����Ҫ�����ز���
        av_log(NULL, AV_LOG_DEBUG, "no resample.\n");
        is->audio_buf = frame->data[0];
        resample_size = av_samples_get_buffer_size(NULL, is->aud_ctx->channels, frame->nb_samples, is->aud_ctx->sample_fmt, 1);
    }

    return resample_size;
}

static void sdl_audio_callback(void *opaque, uint8_t*stream, int len) {
    int len1;
    VideoState *is = (VideoState*) opaque;
    
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
}

static int audio_open(VideoState *is) {
    SDL_AudioSpec wanted_spec, spec;
    SDL_AudioDeviceID audio_dev;

    wanted_spec.channels = is->aud_ctx->channels; // ������
    wanted_spec.freq = is->aud_ctx->sample_rate; // ������
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }
    wanted_spec.format = AUDIO_S16SYS;  // ���ݸ�ʽ
    wanted_spec.silence = 0;            // ����ֵ
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
    SDL_PauseAudioDevice(audio_dev, 0);
    return 0;
}

static int audio_thread(void *arg) {
    int ret;
    VideoState *is = (VideoState*) arg;
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    audio_open(is);

    for (;;) {
        if (is->eof && (is->audioq.nb_packets <= 0)) {
            is->abort_request = 1;
            break;
        }
        packet_queue_get(&is->audioq, pkt);    
        if (!pkt) {
            av_log(NULL, AV_LOG_ERROR, "video packet get error.\n");
            break;
        }
        ret = avcodec_send_packet(is->aud_ctx, pkt);
        if (ret < 0) {
            print_error(is->file_name, ret);
            break;
        }
        av_packet_unref(pkt);
        if (ret < 0) {
            print_error(is->file_name, ret);
            break;
        }
        for(;;) {
            if (is->aud_frame_q.nb_frames >= MAX_FRAME_QUEUE_NB) {
                SDL_Delay(10);
            } else {
                break;
            }
        }
        ret = avcodec_receive_frame(is->aud_ctx, frame);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                continue;
            }
            print_error(is->file_name, ret);
            break;
        }
        frame_queue_put(&is->aud_frame_q, frame);
    }
    av_log(NULL, AV_LOG_ERROR, "audio thread quit.\n");
    return 0;
}

static void video_refresh(VideoState *is) {
    static AVFrame *frame = NULL;
    static SDL_Rect rect;
    static int win_resized = 0;
    if (!is->vid_ctx) {
        av_log(NULL, AV_LOG_ERROR, "video codec context is null, try again later.\n");
        return;
    }
    if (!win_resized) {
        frame = av_frame_alloc();
        rect.x = 0;
        rect.y = 0;
        rect.w = is->vid_ctx->width;
        rect.h = is->vid_ctx->height;
        av_log(NULL, AV_LOG_ERROR, "set window size w %d h %d.\n", rect.w, rect.h);
        SDL_SetWindowSize(window, is->vid_ctx->width, is->vid_ctx->height);
        win_resized = 1;
    }
    
    frame_queue_get(&is->vid_frame_q, frame);
    if (!frame) {
        av_log(NULL, AV_LOG_ERROR, "frame queue get error.\n");
        return;
    }
    SDL_RenderClear(renderer);
    is->sws_ctx = sws_getCachedContext(is->sws_ctx, frame->width, frame->height, frame->format,
        frame->width, frame->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
        sws_scale(is->sws_ctx, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height,
        is->yuv_frame->data, is->yuv_frame->linesize);
    av_frame_unref(frame);
    SDL_UpdateYUVTexture(texture, &rect, is->yuv_frame->data[0], is->yuv_frame->linesize[0],
    is->yuv_frame->data[1], is->yuv_frame->linesize[1],
    is->yuv_frame->data[2], is->yuv_frame->linesize[2]);
    SDL_RenderCopy(renderer, texture, NULL, &rect);
    SDL_RenderPresent(renderer);
}

static int video_thread(void *arg) {
    VideoState *is = (VideoState*)arg;
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    int ret;
    if (!is) {
        av_log(NULL, AV_LOG_ERROR, "user data is NULL.\n");
        return -1;
    }
    for (;;) {
        if (is->eof && (is->videoq.nb_packets <= 0)) {
            is->abort_request = 1;
            break;
        }
        packet_queue_get(&is->videoq, pkt);    
        if (!pkt) {
            av_log(NULL, AV_LOG_ERROR, "video packet get error.\n");
            break;
        }
        ret = avcodec_send_packet(is->vid_ctx, pkt);
        if (ret < 0) {
            print_error(is->file_name, ret);
            break;
        }
        av_packet_unref(pkt);
        if (ret < 0) {
            print_error(is->file_name, ret);
            break;
        }
        for(;;) {
            if (is->vid_frame_q.nb_frames >= MAX_FRAME_QUEUE_NB) {
                SDL_Delay(10);
            } else {
                break;
            }

        }
        ret = avcodec_receive_frame(is->vid_ctx, frame);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                continue;
            }
            print_error(is->file_name, ret);
            break;
        }
        frame_queue_put(&is->vid_frame_q, frame);
    }
    // av_log(NULL, AV_LOG_ERROR, "video thread quit.\n");
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
        is->aud_ctx = avctx;
        is->audio_tid = SDL_CreateThread(audio_thread, "audio_decoder", is);
        if (!is->audio_tid) {
            av_log(NULL, AV_LOG_ERROR, "create audio decoder thread failed, %s.\n", SDL_GetError());
            goto fail;
        }
        /* code */
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->vid_ctx = avctx;
        rational = ic->streams[stream_index]->avg_frame_rate;
        is->interval = 1000 / (rational.num / rational.den);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, 
        avctx->width, avctx->height);
        is->yuv_frame = av_frame_alloc();
        av_image_alloc(is->yuv_frame->data, is->yuv_frame->linesize, avctx->width, avctx->height, AV_PIX_FMT_YUV420P, 1);
        if (!texture) {
            av_log(NULL, AV_LOG_ERROR, "Sdl create texture failed, %s.\n", SDL_GetError());
            goto fail;
        }
        is->video_tid = SDL_CreateThread(video_thread, "video_decoder", is);
        if (!is->video_tid) {
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
    SDL_mutex *wait_mutex;
    SDL_Event event;
    int err;
    AVPacket *pkt = NULL;

    wait_mutex = SDL_CreateMutex();
    if (!wait_mutex) {
        av_log(NULL, AV_LOG_ERROR, "create wait mutex failed, %s.\n", SDL_GetError());
        goto fail;
    }

    err = avformat_open_input(&is->ic, is->file_name, NULL, NULL);
    if (err < 0) {
        print_error(is->file_name, err);
        goto fail;
    }

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
        if (is->abort_request) {
            err = 0;
            break;
        }
        if (is->videoq.nb_packets >= MAX_PACKET_QUEUE_NB || is->audioq.nb_packets >= MAX_PACKET_QUEUE_NB) {
            // av_log(NULL, AV_LOG_DEBUG, "packet buffer is enough, wait 10ms.\n");
            // wait 10ms
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
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
    // av_log(NULL, AV_LOG_ERROR, "read thread quit.\n");
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "quit.\n");
        event.type = SDL_QUIT;
        SDL_PushEvent(&event);
    }
    return 0;
}

VideoState *stream_open(const char *input_file) {
    int err;
    VideoState *is = (VideoState*) av_mallocz(sizeof(VideoState));
    is->interval = 30;
    if (!is) {
        return NULL;
    }
    if (packet_queue_init(&is->videoq) < 0 || 
        packet_queue_init(&is->audioq) < 0 ||
        frame_queue_init(&is->vid_frame_q) < 0 ||
        frame_queue_init(&is->aud_frame_q) < 0) {
        av_log(NULL, AV_LOG_ERROR, "queue init failed.\n");
        goto fail;
    }
    is->continue_read_thread = SDL_CreateCond();
    if (!is->continue_read_thread) {
        av_log(NULL, AV_LOG_ERROR, "create read thread continue cond failed, %s.\n", SDL_GetError());
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

void do_exit() {
    // av_log(NULL, AV_LOG_ERROR, "quit start.\n");
    // SDL_Quit();
    // av_log(NULL, AV_LOG_ERROR, "quit end.\n");
    exit(0);
}

void event_loop(VideoState *is) {
    SDL_Event event;
    event.type = VIDEO_REFRESH_EVENT;
    SDL_PushEvent(&event);
    for (;;) {
        SDL_WaitEvent(&event);
        // av_log(NULL, AV_LOG_DEBUG, "event type: %d.\n", event.type);
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
        case VIDEO_REFRESH_EVENT:
            video_refresh(is);
            if (!is->abort_request || is->vid_frame_q.nb_frames > 0) {
                SDL_Delay(is->interval);
                event.type = VIDEO_REFRESH_EVENT;
                SDL_PushEvent(&event);
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
}
