#include <libavutil/log.h>
#include <libavutil/macros.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <SDL2/SDL.h>

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



void print_error(const char *filename, int err)
{
    char errbuf[128];
    const char *errbuf_ptr = errbuf;

    if (av_strerror(err, errbuf, sizeof(errbuf)) < 0)
        errbuf_ptr = strerror(AVUNERROR(err));
    av_log(NULL, AV_LOG_ERROR, "%s: %s\n", filename, errbuf_ptr);
}

// 获取sdl像素格式和混合模式
void get_sdl_pix_fmt_and_blendmode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode) {
    int i;
    *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
    *sdl_blendmode = SDL_BLENDMODE_NONE;
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


int refresh(void *arg) {
    int framerate = *(int *)(arg);
    int interval = 1000 / framerate;
    SDL_Event event;
    while (1) {
        SDL_Delay(interval);
        event.type = SDL_USEREVENT;
        SDL_PushEvent(&event);
    }
}

#undef main
int main(int argc, char **argv) {
    const char *input_filename;
    int default_width = 640;
    int default_height = 480;


    int                 flags, err, buffer_size;
    int                 st_index[AVMEDIA_TYPE_NB];
    int                 frame_rate;
    uint8_t             *buffer = NULL;
    
    SDL_Window          *window;
    SDL_Renderer        *renderer;
    SDL_RendererInfo    renderer_info = {0};
    SDL_Texture         *texture = NULL;
    Uint32              sdl_pix_fmt;
    SDL_BlendMode       sdl_blendmode;
    SDL_Rect            rect;
    SDL_Event           event;
    SDL_Thread          *thread;

    AVFormatContext     *ic = NULL;
    AVCodecParameters   *codecpar = NULL;
    AVCodecContext      *avctx = NULL;
    const AVCodec       *codec = NULL;
    AVPacket            *pkt = NULL;
    AVFrame             *frame = NULL;
    AVFrame             *yuv_frame = NULL;
    AVRational          rational;
    struct SwsContext   *swsctx = NULL;



    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    av_log_set_level(AV_LOG_DEBUG);
    if (argc < 2) {
        av_log(NULL, AV_LOG_FATAL, "No input file specified.");
        return -1;
    }

    input_filename = argv[1];

    // 打开文件
    err = avformat_open_input(&ic, input_filename, NULL, NULL);
    if (err < 0) {
        print_error(input_filename, err);
        return -1;
    }

    // 查找流信息
    err = avformat_find_stream_info(ic, NULL);
    if (err < 0) {
        print_error(input_filename, err);
        goto exit0;
    }

    // 打印流信息
    av_dump_format(ic, 0, input_filename, 0);

    // 查找视频流
    memset(st_index, -1, sizeof(st_index)); // 要初始化为-1，否则会查找错误
    st_index[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    if (st_index[AVMEDIA_TYPE_VIDEO] < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not find a video stream.");
        goto exit0;
    }

    // 分配解码器上下文空间
    avctx = avcodec_alloc_context3(NULL);
    if (!avctx) {
        av_log(NULL, AV_LOG_ERROR, "error: %s", strerror(ENOMEM));
        goto exit0;
    } 
    
    // 获取视频解码器参数
    codecpar = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]]->codecpar;

    err = avcodec_parameters_to_context(avctx, codecpar);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "avcodec_parameters_to_context failed.");
        goto exit1;
    }

    // 获取解码器
    codec = avcodec_find_decoder(avctx->codec_id);
    if (!codec) {
        av_log(NULL, AV_LOG_ERROR, "Could not find video codec.");
        goto exit1;
    }

    // 打开解码器
    err = avcodec_open2(avctx, codec, NULL);
    if (err < 0) {
        print_error(input_filename, err);
        goto exit1;
    }

    // SDL初始化
    flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
    if (SDL_Init(flags)) {
        av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s", SDL_GetError());
        return -1;
    }
    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

    default_width = avctx->width;
    default_height = avctx->height;
    
    // 创建窗口
    window = SDL_CreateWindow("test", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                      default_width, default_height,
                                      SDL_WINDOW_RESIZABLE|SDL_WINDOWEVENT_SIZE_CHANGED);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (window) {
        // 创建渲染器
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);        
        // renderer = SDL_CreateRenderer(window, -1, 0);
        if (renderer) {
            av_log(NULL, AV_LOG_DEBUG, "create render success.\n");
            if (!SDL_GetRendererInfo(renderer, &renderer_info))
                av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", renderer_info.name);
        }
    }
    if (!window || !renderer || !renderer_info.num_texture_formats) {
        av_log(NULL, AV_LOG_FATAL, "Failed to create window or renderer %s", SDL_GetError());
        return -1;
    }


    pkt = av_packet_alloc();
    if (!pkt) {
        av_log(NULL, AV_LOG_ERROR, "Could not alloc packet.");
        goto exit1;
    }

    frame = av_frame_alloc();
    if (!frame) {
        av_log(NULL, AV_LOG_ERROR, "Could not alloc frame.");
        goto exit1;
    }

    yuv_frame = av_frame_alloc();
    if (!yuv_frame) {
        av_log(NULL, AV_LOG_ERROR, "Could not alloc frame.");
        goto exit1;
    }

    av_log(NULL, AV_LOG_DEBUG, "avctx width %d, height %d.\n", avctx->width, avctx->height);
    buffer_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, avctx->width, avctx->height, 1);
    buffer = (uint8_t *) av_malloc(buffer_size);
    av_image_fill_arrays(yuv_frame->data, yuv_frame->linesize, buffer, AV_PIX_FMT_YUV420P, avctx->width, avctx->height, 1);

    // rect放在循环里会出问题
    rect.x = 0;
    rect.y = 0;
    rect.w = avctx->width;
    rect.h = avctx->height;

    rational = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]]->avg_frame_rate;
    frame_rate = rational.num / rational.den;
    av_log(NULL, AV_LOG_ERROR, "frame rate: %d", frame_rate);
    thread = SDL_CreateThread(refresh, "refresh", (void *)&frame_rate);
    if (!thread) {
        av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread failed.\n");
        return -1;
    }

    for (;;) {
        av_packet_unref(pkt);
        err = av_read_frame(ic, pkt);
        if (err < 0) {
            if (err == AVERROR_EOF) {
                break;
            }
            // if (ic->pb && ic->pb->error) {
            //     break;
            // }
            print_error(input_filename, err);
            continue;
        }

        // 得到video packet
        if (pkt->stream_index != st_index[AVMEDIA_TYPE_VIDEO]) {
            av_log(NULL, AV_LOG_DEBUG, "not video packet.\n");
            continue;
        }

        // 向解码器送es流
        err = avcodec_send_packet(avctx, pkt);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "send packet failed.\n");
            break;
        }

        // 获取解码后的数据，对于video来说是yuv流
        err = avcodec_receive_frame(avctx, frame);
        if (err < 0) {
            if (err == AVERROR(EAGAIN)) {
                continue;
            }
            break;
        }

loop:
        // 如果不处理sdl event，会卡死
        SDL_WaitEvent(&event);
        // av_log(NULL, AV_LOG_ERROR, "event type: %d\n", event.type);
        if (event.type != SDL_USEREVENT) {
            goto loop;
        }

        // 显示当前一帧
        SDL_RenderClear(renderer);
        if (!texture) {
            // get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
            // if (sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN) 
            // texture = SDL_CreateTexture(renderer, sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ABGR8888 : sdl_pix_fmt,
            //                             SDL_TEXTUREACCESS_STREAMING, frame->width, frame->height);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, frame->width, frame->height);
            if (!texture) {
                av_log(NULL, AV_LOG_ERROR, "SDL_CreateTexture failed.");
                break;
            }
        }
        if (!swsctx) {
            swsctx = sws_getCachedContext(swsctx, frame->width, frame->height, frame->format,
                                                frame->width, frame->height, AV_PIX_FMT_YUV420P,
                                                SWS_BICUBIC, NULL, NULL, NULL);
        }

        sws_scale(swsctx, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height, yuv_frame->data, yuv_frame->linesize);
        SDL_UpdateYUVTexture(texture, &rect, yuv_frame->data[0], yuv_frame->linesize[0],
                                            yuv_frame->data[1], yuv_frame->linesize[1],
                                            yuv_frame->data[2], yuv_frame->linesize[2]);
        // SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, &rect);
        SDL_RenderPresent(renderer);
    }

    if (texture) {
        SDL_DestroyTexture(texture);
    }
    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }

exit1:
    avcodec_close(avctx);
exit0:
    avformat_close_input(&ic);
    return 0;
}