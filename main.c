#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <SDL2/SDL.h>
#undef main

int main(int argc, char *argv[]) {
  av_register_all();

  // Initialize SDL
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
    fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
    exit(1);
  }

  AVFormatContext *pFormatCtx = NULL;

  // Open video file
  if (avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) != 0) {
    return -1; // Couldn`t open file
  }

  // Retrieve stream information
  if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
    return -1; // Couldn`t find stream information
  }

  // Dump information about file onto standard error
  av_dump_format(pFormatCtx, 0, argv[1], 0);

  int i;
  AVCodecContext  *pCodecCtxOrig = NULL;
  AVCodecContext  *pCodecCtx = NULL;

  // Find the first video stream
  int videoStream = -1;
  for (i = 0; i < pFormatCtx->nb_streams; ++i) {
    if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
      videoStream = i;
      break;
    }
  }
  if (videoStream == -1) {
    return -1; // Didn`t find a video stream
  }

  // Get a pointer to the codec contex for the video stream
  pCodecCtxOrig = pFormatCtx->streams[videoStream]->codec;

  AVCodec *pCodec = NULL;

  // Find the decoder for the video stream
  pCodec = avcodec_find_decoder(pCodecCtxOrig->codec_id);
  if (pCodec == NULL) {
    fprintf(stderr, "Unsupported codec!\n");
    return -1; // Codec not found
  }

  // Copy context
  pCodecCtx = avcodec_alloc_context3(pCodec);
  if (avcodec_copy_context(pCodecCtx, pCodecCtxOrig) != 0) {
    fprintf(stderr, "Couldn`t copy codec context");
    return -1; // Error copying codec context
  }

  // Open codec
  if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
    return -1; // Could not open codec
  }

  AVFrame *pFrame = NULL;

  // Allocate video frame
  pFrame = av_frame_alloc();

  SDL_Window *window;
  window = SDL_CreateWindow("Media Player",
                            SDL_WINDOWPOS_UNDEFINED,
                            SDL_WINDOWPOS_UNDEFINED,
                            pCodecCtx->width,
                            pCodecCtx->height,
                            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  if (!window) {
    fprintf(stderr, "SDL: could not create window by SDL - exiting\n");
  }

  SDL_Renderer *renderer;
  renderer = SDL_CreateRenderer(window, -1, 0);
  if (!renderer) {
    fprintf(stderr, "SDL: could not create Renderer by SDL - exiting\n");
  }

  SDL_Texture *texture;
  texture = SDL_CreateTexture(renderer,
                              SDL_PIXELFORMAT_IYUV,
                              SDL_TEXTUREACCESS_STREAMING,
                              pCodecCtx->width,
                              pCodecCtx->height);

  int frameFinished;
  AVPacket packet;
  i = 0;
  SDL_Rect rect;
  SDL_Event event;
  while (av_read_frame(pFormatCtx, &packet) >= 0) {
    // Is this a packet from the video stream?
    if (packet.stream_index == videoStream) {
      // Decode video frame
      avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);

      // Did we get a video frame?
      if (frameFinished) {
        SDL_UpdateYUVTexture(texture, NULL,
                             pFrame->data[0], pFrame->linesize[0],
                             pFrame->data[1], pFrame->linesize[1],
                             pFrame->data[2], pFrame->linesize[2]);

        rect.x = 0;
        rect.y = 0;
        rect.w = pCodecCtx->width;
        rect.h = pCodecCtx->height;

        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, &rect);
        SDL_RenderPresent(renderer);
      }
    }

    av_free_packet(&packet);
    SDL_PollEvent(&event);
    switch(event.type) {
      case SDL_QUIT:
        SDL_Quit();
        exit(0);
      default:
        break;
    }
  }

  // Free the packet that was allocated by av_read_frame
  av_free_packet(&packet);

  // Free the YUV frame
  av_free(pFrame);

  // Close the codecs
  avcodec_close(pCodecCtx);
  avcodec_close(pCodecCtxOrig);

  // Close the video file
  avformat_close_input(&pFormatCtx);

  return 0;
}