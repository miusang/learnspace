#include <libavutil/log.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_id.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#include <SDL2/SDL.h>


typedef struct PacketQueue {
    AVPacketList *head;
    AVPacketList *tail;
    int nb_packets;
    int size;
    int eof;
    SDL_mutex *mutex;
    SDL_cond  *cond;
} PacketQueue;

typedef struct AudioParams {
    int freq;
    int channels;
    int frame_size;
    int bytes_per_sec;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
} AudioParams;


static int play_end = 0; // 播放结束标志，解码后的缓冲队列size为0是，置1;
static int need_resample = 0;
static uint8_t *resample_buf = NULL; // 重采样后的音频数据（PCM）缓冲区。
static int resample_buf_len;
static PacketQueue audioq;
struct AudioParams audio_src;
static struct SwrContext *swr_ctx = NULL;

static int push_num = 0;
static int pop_num = 0;




void print_error(const char *filename, int err)
{
    av_log(NULL, AV_LOG_ERROR, "print error.\n");
    char errbuf[128];
    const char *errbuf_ptr = errbuf;

    if (av_strerror(err, errbuf, sizeof(errbuf)) < 0)
        errbuf_ptr = strerror(AVUNERROR(err));
    av_log(NULL, AV_LOG_ERROR, "%s: %s\n", filename, errbuf_ptr);
}

void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

// 此处没考虑内存buffer的限制
void packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    if (av_packet_make_refcounted(pkt) < 0) {
        return;
    }
    AVPacketList *list = (AVPacketList *) av_malloc(sizeof(AVPacketList));
    list->pkt = *pkt;
    list->next = NULL;

    SDL_LockMutex(q->mutex);
    if (!q->tail) {
        q->head = list;
    } else {
        q->tail->next = list;
    }
    q->tail = list;
    q->nb_packets++;
    q->size += pkt->size;
    push_num++;
    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
}

int packet_queue_get(PacketQueue *q, AVPacket *pkt) {
    AVPacketList *list;
    int ret = 0;
    SDL_LockMutex(q->mutex);
    for (;;) {
        list = q->head;
        if (list) {
            q->head = list->next;
            if (!q->head) {
                q->tail = NULL;
            }
            q->nb_packets--;
            q->size -= list->pkt.size;
            *pkt = list->pkt;
            av_free(list);
            pop_num++;
            break;
        } else {
            if (q->eof) {
                ret = -1;
                // av_log(NULL, AV_LOG_ERROR, "read eof and packet queue empty.\n");
                break;
            }
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}


// 解码
int audio_decode_frame(AVCodecContext *avctx, uint8_t *audio_buf, int buf_size) {
    int err, packet_pending;

    int nb_resamples;
    int cp_buf_len = 0;
    uint8_t *cp_buf = NULL;
    
    
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    packet_pending = 0;
    // av_log(NULL, AV_LOG_ERROR, "error %s\n", SDL_GetError());
    for (;;) {
        err = avcodec_receive_frame(avctx, frame);
        if (err < 0) {
            if (err == AVERROR(EAGAIN)) {
                packet_pending = 1;
            } else {
                av_log(NULL, AV_LOG_ERROR, "try again.\n");
                print_error("test", err);
                // play_end = 1;
                break;
            }            
        } else {
            if (!need_resample && (frame->format != audio_src.fmt ||
                                   frame->channel_layout != audio_src.channel_layout ||
                                   frame->sample_rate != audio_src.freq)) {
                need_resample = 1;
                av_log(NULL, AV_LOG_ERROR, "need resample.\n");
                swr_ctx = swr_alloc_set_opts(NULL, audio_src.channel_layout, audio_src.fmt, audio_src.freq,
                                            frame->channel_layout, frame->format, frame->sample_rate, 0, NULL);
                if (!swr_ctx || swr_init(swr_ctx) < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Create sample rate converter failed.\n");
                    return -1;
                }
            }
            // 需要进行重采样
            if (swr_ctx) {
                const uint8_t **in = (const uint8_t**) frame->extended_data;
                uint8_t **out = &resample_buf;
                // 此处为啥多加256？
                int nb_samples = (int64_t)frame->nb_samples * audio_src.freq / frame->sample_rate + 256;
                int out_size = av_samples_get_buffer_size(NULL, audio_src.channels, nb_samples, audio_src.fmt, 0);
                if (resample_buf == NULL) {
                    av_fast_malloc(&resample_buf, &resample_buf_len, out_size);
                }
                // av_log(NULL, AV_LOG_DEBUG, "convert begin.\n");
                nb_resamples = swr_convert(swr_ctx, out, nb_samples, in, frame->nb_samples);
                // av_log(NULL, AV_LOG_DEBUG, "convert end.\n");
                cp_buf = resample_buf;
                if (!cp_buf) {
                    av_log(NULL, AV_LOG_ERROR, "resample buf is null.\n");
                    break;
                }
                cp_buf_len = nb_resamples * audio_src.channels * av_get_bytes_per_sample(audio_src.fmt);
            } else { // 不需要进行重采样
                av_log(NULL, AV_LOG_DEBUG, "no resample.\n");
                cp_buf = frame->data[0];
                cp_buf_len = av_samples_get_buffer_size(NULL, avctx->channels, frame->nb_samples, avctx->sample_fmt, 1);
            }
            if (cp_buf_len > buf_size) {
                av_log(NULL, AV_LOG_ERROR, "cp buf len is too large.\n");
            }
            memcpy(audio_buf, cp_buf, cp_buf_len);
            // av_log(NULL, AV_LOG_DEBUG, "copy to audio buffer， len %d\n.", cp_buf_len);
            return cp_buf_len;
        }

        if (packet_pending) {
            // av_log(NULL, AV_LOG_DEBUG, "penddinng packet.\n");
            if (packet_queue_get(&audioq, pkt) < 0) {
                play_end = 1;
                break;
            }
            err = avcodec_send_packet(avctx, pkt);
            av_packet_unref(pkt);
            if (err < 0) {
                av_log(NULL, AV_LOG_ERROR, "send packet error.");
                break;
            }
        }
    }
    return -1;
}

// 播放回调，从解码缓冲队列获取解码后的数据
void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
    // if (play_end) { // 如果直接推出会有杂音，要输出静音
    //     return;
    // }
    AVCodecContext *avctx = (AVCodecContext *)opaque;
    static uint8_t audio_buf[192000 * 3 / 2];
    static int audio_buf_len = 0; // 解码后的总长度
    static int used_len = 0; // 已经播放的长度
    int rest_len; // 剩余长度
    
    // av_log(NULL, AV_LOG_ERROR, "callback.\n");
    while (len > 0) { // 退出循环的条件为，stream缓冲区填满，即 len == 0
        if (used_len >= audio_buf_len) {
            audio_buf_len = audio_decode_frame(avctx, audio_buf, sizeof(audio_buf));
            if (audio_buf_len < 0) {
                // if (play_end) {
                //     break;
                // }
                av_log(NULL, AV_LOG_ERROR, "audio decode frame len %d.\n", audio_buf_len);
                audio_buf_len = 1024;
                memset(audio_buf, 0, audio_buf_len);
            } 
            used_len = 0;
        }
        rest_len = audio_buf_len - used_len;
        if (rest_len > len) {
            rest_len = len;
        }
        memcpy(stream, audio_buf + used_len, rest_len);
        len -= rest_len;
        stream += rest_len;
        used_len += rest_len;
    }
}

#undef main
int main(int argc, char **argv) {
    
    const char              *input_file = NULL;
    int                     err;
    int                     st_index[AVMEDIA_TYPE_NB];

    AVFormatContext         *ic = NULL;
    AVCodecContext          *avctx = NULL;
    const AVCodec           *codec = NULL;
    AVPacket                *pkt = NULL;

    SDL_AudioSpec           wanted_spec, spec;
    SDL_AudioDeviceID       audio_dev;
    SDL_Thread              *decoder_tid;

    av_log_set_level(AV_LOG_DEBUG);

    if (argc < 2) {
        av_log(NULL, AV_LOG_ERROR, "No input file specified.\n");
        return -1;
    }
    input_file = argv[1];

    err = avformat_open_input(&ic, input_file, NULL, NULL);
    if (err < 0) {
        print_error(input_file, err);
        return -1;
    }

    err = avformat_find_stream_info(ic, NULL);
    if (err < 0) {
        print_error(input_file, err);
        return -1;
    }

    av_dump_format(ic, 0, input_file, 0);

    memset(st_index, -1, sizeof(st_index));
    st_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, st_index[AVMEDIA_TYPE_AUDIO], -1, NULL, 0);

    if (st_index[AVMEDIA_TYPE_AUDIO] < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not find audio stream.\n");
        return -1;
    }

    avctx = avcodec_alloc_context3(NULL);
    if (!avctx) {
        av_log(NULL, AV_LOG_ERROR, "avcode alloc failed.\n");
        return -1;
    }

    err = avcodec_parameters_to_context(avctx, ic->streams[st_index[AVMEDIA_TYPE_AUDIO]]->codecpar);
    if (err < 0) {
        print_error(input_file, err);
        return -1;
    }

    codec = avcodec_find_decoder(avctx->codec_id);
    if (!codec) {
        av_log(NULL, AV_LOG_ERROR, "No decoder found for %s\n.", avcodec_get_name(avctx->codec_id));
        return -1;
    }

    err = avcodec_open2(avctx, codec, NULL);
    if (err < 0) {
        print_error(input_file, err);
        return -1;
    }
    if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
        SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE","1", 1);
    err = SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_VIDEO);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "sdl init failed.\n");
        return -1;
    }
    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);

    wanted_spec.channels = avctx->channels; // 声道数
    wanted_spec.freq =  avctx->sample_rate; // 采样率
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }
    wanted_spec.format = AUDIO_S16SYS;  // 数据格式
    wanted_spec.silence = 0;            // 静音值
    wanted_spec.samples = 1024;
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = avctx;
    
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!audio_dev) {
        av_log(NULL, AV_LOG_ERROR, "SDL_OpenAudioDevice failed, %s\n", SDL_GetError());
        return -1;
    }
    // if (SDL_OpenAudio(&wanted_spec, &spec) < 0) { // windows下没有声音
    //     av_log(NULL, AV_LOG_ERROR, "SDL_OpenAudioDevice failed, %s\n", SDL_GetError());
    //     return -1;
    // }
    

    audio_src.fmt = AV_SAMPLE_FMT_S16;
    audio_src.freq = spec.freq;
    av_log(NULL, AV_LOG_DEBUG, "sample rate %d\n", spec.freq);
    audio_src.channel_layout = av_get_default_channel_layout(spec.channels);
    audio_src.channels = spec.channels;
    audio_src.frame_size = av_samples_get_buffer_size(NULL, spec.channels, 1, audio_src.fmt, 1);
    audio_src.bytes_per_sec = av_samples_get_buffer_size(NULL, spec.channels, audio_src.freq, audio_src.fmt, 1);
    if (audio_src.bytes_per_sec <= 0 || audio_src.frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed.\n");
        return -1;
    }


    packet_queue_init(&audioq);
    // SDL_PauseAudio(0);
    SDL_PauseAudioDevice(audio_dev, 0);
    pkt = av_packet_alloc();
    
    // decoder_tid = SDL_CreateThread(audio_thread, "audio_decoder", avctx);
    // if (!decoder_tid) {
    //     av_log(NULL, AV_LOG_ERROR, "audio decoder thead create failed.\n");
    //     return -1;
    // }


    for (;;) {
        err = av_read_frame(ic, pkt);
        if (err < 0) {
            audioq.eof = 1;
            break;
        }
        if (pkt->stream_index != st_index[AVMEDIA_TYPE_AUDIO]) { 
            av_packet_unref(pkt);
            continue;
        }
        packet_queue_put(&audioq, pkt);
        // av_log(NULL, AV_LOG_DEBUG, "put packet.\n");
    }

    for (;;) {
        if (play_end) {
            av_log(NULL, AV_LOG_ERROR, "play end.\n");
            break;
        }
        SDL_Delay(500);
    }
    av_log(NULL, AV_LOG_ERROR, "push num: %d, pop num: %d \n", push_num, pop_num);

    return 0;
}