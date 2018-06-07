#include <stdio.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#include <unistd.h>

#define ENABLE_LOGS 1

#ifdef ENABLE_LOGS
    #define LOG(...) printf(__VA_ARGS__)
#else
    #define LOG(...) 
#endif // ENABLE_LOGS

void dumpDictionaryToConsole(AVDictionary* pDict) {
    if (!pDict)
        return;
    AVDictionaryEntry* pEntry = NULL;
    while ((pEntry = av_dict_get(pDict, "", pEntry, AV_DICT_IGNORE_SUFFIX)))
        LOG("%s:%s\n", pEntry->key, pEntry->value);
}

void saveFrameToDisk(AVFrame* pFrame, int width, int height, int frameNumber) {
    if (!pFrame || !width || !height)
        return;

    char fileName[32];
    memset(fileName, 0, sizeof(fileName));
    snprintf(fileName, sizeof(fileName), "frame%d.ppm", frameNumber);

    FILE* pFile = fopen(fileName, "wb");
    if (!pFile)
        return;

    // Write PPM header
    fprintf(pFile, "P6\n%d %d\n255\n", width, height);

    // Write pixel data
    for (int i = 0; i < height; ++i)
        fwrite(pFrame->data[0] + i * pFrame->linesize[0], 1, width * 3, pFile); // 3 - number of bytes - RGB

    fclose(pFile);
}

int saveNumFramesToDisk(const char* url, int numFrames) {
    if (!url || numFrames <= 0)
        return -1;

    int res = AVERROR_UNKNOWN;

    AVFormatContext* pFormatContext = NULL;
    AVCodecContext* pCodecContext = NULL;

    AVDictionary** dicts = NULL;
    int dictsSize = 0;

    AVFrame* pFrameYUV = NULL;
    AVFrame* pFrameRGB = NULL;

    uint8_t* buffer = NULL;

    do {
        av_register_all();

        res = avformat_open_input(&pFormatContext, url, NULL, NULL);
        if (res < 0) {
            LOG("avformat_open_input() returned \"%s\"\n", av_err2str(res));
            break;
        }

        //
        // Fill options array for a each stream in a container
        //

        dictsSize = pFormatContext->nb_streams;
        if (!dictsSize) {
            LOG("No a single stream in a container\n");
            break;
        }

        dicts = av_malloc(dictsSize * sizeof(AVDictionary*));
        if (!dicts) {
            LOG("Failed to malloc() array for %d AVDictionary pointers\n", dictsSize);
            break;
        }

        for (int i = 0; i < dictsSize; ++i) {
            AVDictionary* pDict = NULL;
            av_dict_set(&pDict, "some", "property", 0);
            dicts[i] = pDict;
        }

        res = avformat_find_stream_info(pFormatContext, dicts);
        if (res < 0) {
            LOG("avformat_find_stream_info() returned \"%s\"\n", av_err2str(res));
            break;
        }

        //
        // Dump format details and options that were not set for each stream
        //

        av_dump_format(pFormatContext, 0, url, 0);
        for (int i = 0; i < dictsSize; ++i)
            dumpDictionaryToConsole(dicts[i]);

        int videoStreamIndex = -1;
        for (int i = 0; i < pFormatContext->nb_streams; ++i) {
            if (AVMEDIA_TYPE_VIDEO == pFormatContext->streams[i]->codec->codec_type) {
                videoStreamIndex = i;
                break;
            }
        }
        if (-1 == videoStreamIndex) {
            LOG("Video stream was not found\n");
            break;
        }

        AVCodecContext* pCodecContextOrig = pFormatContext->streams[videoStreamIndex]->codec;
        AVCodec* pCodec = avcodec_find_decoder(pCodecContextOrig->codec_id);
        if (!pCodec) {
            LOG("Unable to find decoder %d\n", pCodecContextOrig->codec_id);
            break;
        }

        //
        // Copy codec context, because we cannot use AVCodecContext from video stream directly
        //

        pCodecContext = avcodec_alloc_context3(pCodec);
        if (!pCodecContext) {
            LOG("avcodec_alloc_context3() failed\n");
            break;
        }

        res = avcodec_copy_context(pCodecContext, pCodecContextOrig);
        if (res < 0) {
            LOG("avcodec_copy_context() failed with %s\n", av_err2str(res));
            break;
        }

        res = avcodec_open2(pCodecContext, pCodec, NULL);
        if (res < 0) {
            LOG("avcodec_open2() failed with %s\n", av_err2str(res));
            break;
        }

        //
        // Storing the data
        //

        pFrameRGB = av_frame_alloc();
        if (!pFrameRGB) {
            LOG("av_frame_alloc() failed for RGB frame\n");
            break;
        }

        int numBytes = avpicture_get_size(AV_PIX_FMT_RGB24, pCodecContext->width, pCodecContext->height);
        if (numBytes <= 0) {
            LOG("avpicture_get_size() failed\n");
            break;
        }

        buffer = av_malloc(numBytes * sizeof(uint8_t));
        if (!buffer) {
            LOG("av_malloc() failed\n");
            break;
        }

        // Assign appropriate parts of buffer to image planes in pFrameRGB
        // Note that pFrameRGB is an AVFrame, but AVFrame is a superset of AVPicture
        res = avpicture_fill((AVPicture*)pFrameRGB, buffer, AV_PIX_FMT_RGB24, pCodecContext->width, pCodecContext->height);
        if (res < 0) {
            LOG("avpicture_fill() failed with %s\n", av_err2str(res));
            break;
        }

        struct SwsContext* pSwsContext = sws_getContext(pCodecContext->width,
                                                       pCodecContext->height,
                                                       pCodecContext->pix_fmt,
                                                       pCodecContext->width,
                                                       pCodecContext->height,
                                                       AV_PIX_FMT_RGB24,
                                                       SWS_BILINEAR,
                                                       NULL,
                                                       NULL,
                                                       NULL);
        if (!pSwsContext) {
            LOG("sws_getContext() failed\n");
            break;
        }

        AVPacket packet;
        memset(&packet, 0, sizeof(packet));

        pFrameYUV = av_frame_alloc();
        if (!pFrameYUV) {
            LOG("av_frame_alloc() failed for YUV frame\n");
            break;
        }

        int i = 0;
        int frameFinished = 0;
        for(;;) { // av_read_frame() loop
            av_packet_unref(&packet);
			
            res = av_read_frame(pFormatContext, &packet);
            if (res < 0) {
                LOG("av_read_frame() returned %s\n", av_err2str(res));
                break;
            }

            if (videoStreamIndex != packet.stream_index)
                continue;

            // Actually decode video frame and place it to pFrameYUV
            res = avcodec_decode_video2(pCodecContext, pFrameYUV, &frameFinished, &packet);
            if (res < 0) {
                LOG("avcodec_decode_video2() failed with %s\n", av_err2str(res));
                continue;
            }

            if (!frameFinished)
                continue;

            // Convert the image from its native format YUV to RGB
            res = sws_scale(pSwsContext,
                            (const uint8_t* const *)pFrameYUV->data,
                            pFrameYUV->linesize,
                            0,
                            pCodecContext->height,
                            pFrameRGB->data,
                            pFrameRGB->linesize);
            if (res <= 0) {
                LOG("sws_scale() failed\n");
                continue;
            }

            if (++i > numFrames)
                break;

            // Save first numFrames frames to disk
            LOG("Saving frame %d of %d\n", i, numFrames);
            saveFrameToDisk(pFrameRGB, pCodecContext->width, pCodecContext->height, i);

        } // for(;;) av_read_frame()

        LOG("av_read_frame() loop finished\n");
    } while (0);

    //
    // Cleanup
    //

    if (buffer)
        av_free(buffer);

    if (pFrameYUV)
        av_frame_free(&pFrameYUV);

    if (pFrameRGB)
        av_frame_free(&pFrameRGB);

    if (pCodecContext)
        avcodec_free_context(&pCodecContext);

    if (dicts && dictsSize) {
        for (int i = 0; i < dictsSize; ++i)
            av_dict_free(&dicts[i]);
        av_free(dicts);
        dicts = NULL;
        dictsSize = 0;
    }

    if (pFormatContext) {
        avformat_close_input(&pFormatContext);
        LOG("avformat_close_input() returned.\n");
    }

    return res;
}

int outputVideoFramesToWindow(const char* url) {
    if (!url)
        return -1;

    int res = AVERROR_UNKNOWN;

    AVFormatContext* pFormatContext = NULL;
    AVCodecContext* pCodecContext = NULL;
    AVFrame* pFrame = NULL;

    SDL_Event event;
    SDL_Window* hWindow = NULL;
    SDL_Renderer* hRenderer = NULL;
    SDL_Texture* hTexture = NULL;

    uint8_t *yPlane = NULL, *uPlane = NULL, *vPlane = NULL;

    do {
        av_register_all();

        res = SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO | SDL_INIT_VIDEO);
        if (res < 0) {
            LOG("SDL_Init() failed with %s\n", SDL_GetError());
            break;
        }

        res = avformat_open_input(&pFormatContext, url, NULL, NULL);
        if (res < 0) {
            LOG("avformat_open_input() returned \"%s\"\n", av_err2str(res));
            break;
        }

        res = avformat_find_stream_info(pFormatContext, NULL);
        if (res < 0) {
            LOG("avformat_find_stream_info() returned \"%s\"\n", av_err2str(res));
            break;
        }

        av_dump_format(pFormatContext, 0, url, 0);

        int videoStreamIndex = -1;
        for (int i = 0; i < pFormatContext->nb_streams; ++i) {
            if (AVMEDIA_TYPE_VIDEO == pFormatContext->streams[i]->codec->codec_type) {
                videoStreamIndex = i;
                break;
            }
        }
        if (-1 == videoStreamIndex) {
            LOG("Video stream was not found\n");
            break;
        }

        AVCodecContext* pCodecContextOrig = pFormatContext->streams[videoStreamIndex]->codec;
        AVCodec* pCodec = avcodec_find_decoder(pCodecContextOrig->codec_id);
        if (!pCodec) {
            LOG("Unable to find decoder %d\n", pCodecContextOrig->codec_id);
            break;
        }

        // Copy codec context, because we cannot use AVCodecContext from video stream directly
        pCodecContext = avcodec_alloc_context3(pCodec);
        if (!pCodecContext) {
            LOG("avcodec_alloc_context3() failed\n");
            break;
        }

        res = avcodec_copy_context(pCodecContext, pCodecContextOrig);
        if (res < 0) {
            LOG("avcodec_copy_context() failed with %s\n", av_err2str(res));
            break;
        }

        res = avcodec_open2(pCodecContext, pCodec, NULL);
        if (res < 0) {
            LOG("avcodec_open2() failed with %s\n", av_err2str(res));
            break;
        }

        hWindow = SDL_CreateWindow("uPlayer",
                                   SDL_WINDOWPOS_UNDEFINED,
                                   SDL_WINDOWPOS_UNDEFINED,
                                   pCodecContext->width,
                                   pCodecContext->height,
                                   0);
        if (!hWindow) {
            LOG("SDL_CreateWindow() failed with %s\n", SDL_GetError());
            break;
        }

        hRenderer = SDL_CreateRenderer(hWindow, -1, 0);
        if (!hRenderer) {
            LOG("SDL_CreateRenderer() failed with %s\n", SDL_GetError());
            break;
        }

        hTexture = SDL_CreateTexture(hRenderer,
                                     SDL_PIXELFORMAT_YV12,
                                     SDL_TEXTUREACCESS_STREAMING,
                                     pCodecContext->width,
                                     pCodecContext->height);
        if (!hTexture) {
            LOG("SDL_CreateTexture() failed with %s\n", SDL_GetError());
            break;
        }

        // Initialize SWS context for software scaling
        struct SwsContext* pSwsContext = sws_getContext(pCodecContext->width,
                                                       pCodecContext->height,
                                                       pCodecContext->pix_fmt,
                                                       pCodecContext->width,
                                                       pCodecContext->height,
                                                       AV_PIX_FMT_YUV420P,
                                                       SWS_BILINEAR,
                                                       NULL,
                                                       NULL,
                                                       NULL);
        if (!pSwsContext) {
            LOG("sws_getContext() failed\n");
            break;
        }

        AVPacket packet;
        memset(&packet, 0, sizeof(packet));

        pFrame = av_frame_alloc();
        if (!pFrame) {
            LOG("av_frame_alloc() failed for YUV frame\n");
            break;
        }

        // Set up YV12 pixel array (12 bits per array)
        size_t yPlaneSize = pCodecContext->width * pCodecContext->height;
        size_t uvPlaneSize = (pCodecContext->width * pCodecContext->height) / 4;
        yPlane = av_malloc(yPlaneSize);
        uPlane = av_malloc(uvPlaneSize);
        vPlane = av_malloc(uvPlaneSize);
        if (!yPlane || !uPlane || !vPlane) {
            LOG("av_malloc() failed for pixel buffers\n");
            break;
        }

        int stopped = 0;
        int frameFinished = 0;
        int uvPitch = pCodecContext->width / 2;
        for(;;) { // av_read_frame() loop
            av_packet_unref(&packet);

            if (stopped) {
                LOG("User stopped playback\n");
                break;
            }

            res = av_read_frame(pFormatContext, &packet);
            if (res < 0) {
                LOG("av_read_frame() returned %s\n", av_err2str(res));
                break;
            }

            if (videoStreamIndex == packet.stream_index) {
                // Decode video frame and place it to pFrame
                res = avcodec_decode_video2(pCodecContext, pFrame, &frameFinished, &packet);
                if (res >= 0) {
                    if (frameFinished) {
                        AVPicture picture;
                        picture.data[0] = yPlane;
                        picture.data[1] = uPlane;
                        picture.data[2] = vPlane;
                        picture.linesize[0] = pCodecContext->width;
                        picture.linesize[1] = uvPitch;
                        picture.linesize[2] = uvPitch;

                        // Convert the into YUV format that SDL uses
                        res = sws_scale(pSwsContext,
                                        (const uint8_t* const *)pFrame->data,
                                        pFrame->linesize,
                                        0,
                                        pCodecContext->height,
                                        picture.data,
                                        picture.linesize);
                        if (res > 0) {
                            res = SDL_UpdateYUVTexture(hTexture,
                                                       NULL,
                                                       yPlane,
                                                       pCodecContext->width,
                                                       uPlane,
                                                       uvPitch,
                                                       vPlane,
                                                       uvPitch);
                            if (0 == res) {
                                SDL_RenderClear(hRenderer);
                                SDL_RenderCopy(hRenderer, hTexture, NULL, NULL);
                                SDL_RenderPresent(hRenderer);
                                usleep(30 * 1000);
                            } // if (SDL_UpdateYUVTexture() == 0)
                            else
                                LOG("SDL_UpdateYUVTexture() failed with %d\n", res);

                        } // if (sws_scale() > 0)
                        else
                            LOG("sws_scale() failed\n");

                    } // if (frameFinished)

                } // if (avcodec_decode_video2() >= 0)
                else
                    LOG("avcodec_decode_video2() failed with %s\n", av_err2str(res));

            } // if (videoStreamIndex == packet.stream_index)

            SDL_PollEvent(&event);
            switch (event.type) {
            case SDL_QUIT: {
                SDL_DestroyTexture(hTexture);
                SDL_DestroyRenderer(hRenderer);
                SDL_DestroyWindow(hWindow);
                SDL_Quit();
                stopped = 1;
                break;
            }
            default:
                break;
            }

        } // for(;;) av_read_frame() loop

        LOG("av_read_frame() loop finished\n");
    } while (0);

    //
    // Cleanup
    //
    if (yPlane)
        av_free(yPlane);

    if (uPlane)
        av_free(uPlane);

    if (vPlane)
        av_free(vPlane);

    if (hTexture)
        SDL_DestroyTexture(hTexture);

    if (hRenderer)
        SDL_DestroyRenderer(hRenderer);

    if (hWindow)
        SDL_DestroyWindow(hWindow);

    if (pFrame)
        av_frame_free(&pFrame);

    if (pCodecContext)
        avcodec_free_context(&pCodecContext);

    if (pFormatContext) {
        avformat_close_input(&pFormatContext);
        LOG("avformat_close_input() returned.\n");
    }

    return res;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        LOG("Usage:\n    uplayer file_name\n");
        return -1;
    }

    int res = -1;

    //res = saveNumFramesToDisk(argv[1], 20);

    res = outputVideoFramesToWindow(argv[1]);

    return res;
}
