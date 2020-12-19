#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/avstring.h>
#include <SDL2/SDL.h>
#undef main

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define VIDEO_PICTURE_QUEUE_SIZE 1

SDL_Window *window;
SDL_Renderer *renderer;
SDL_Texture *texture;

typedef struct PacketQueue {
  AVPacketList *first_pkt, *last_pkt;
  int nb_packets;
  int size;
  SDL_mutex *mutex;
  SDL_cond *cond;
} PacketQueue;


typedef struct VideoPicture {
  AVFrame *frame;
  int width, height; /* source height & width */
  int allocated;
} VideoPicture;

typedef struct VideoState {
  AVFormatContext *pFormatCtx;
  int             videoStream, audioStream;
  AVStream        *audio_st;
  AVCodecContext  *audio_ctx;
  PacketQueue     audioq;
  uint8_t         audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
  unsigned int    audio_buf_size;
  unsigned int    audio_buf_index;
  AVFrame         audio_frame;
  AVPacket        audio_pkt;
  uint8_t         *audio_pkt_data;
  int             audio_pkt_size;
  AVStream        *video_st;
  AVCodecContext  *video_ctx;
  PacketQueue     videoq;

  VideoPicture    pictq[VIDEO_PICTURE_QUEUE_SIZE];
  int             pictq_size, pictq_rindex, pictq_windex;
  SDL_mutex       *pictq_mutex;
  SDL_cond        *pictq_cond;

  SDL_Thread      *parse_tid;
  SDL_Thread      *video_tid;

  char            filename[1024];
  int             quit;
} VideoState;

SDL_mutex *screen_mutex;

VideoState *global_video_state;

void packet_queue_init(PacketQueue *q) {
  memset(q, 0, sizeof(PacketQueue));
  q->mutex = SDL_CreateMutex();
  q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
  AVPacketList *pkt1;
  if(av_dup_packet(pkt) < 0) {
    return -1;
  }
  pkt1 = av_malloc(sizeof(AVPacketList));
  if (!pkt1)
    return -1;
  pkt1->pkt = *pkt;
  pkt1->next = NULL;

  SDL_LockMutex(q->mutex);

  if (!q->last_pkt)
    q->first_pkt = pkt1;
  else
    q->last_pkt->next = pkt1;
  q->last_pkt = pkt1;
  q->nb_packets++;
  q->size += pkt1->pkt.size;

  SDL_CondSignal(q->cond);
  SDL_UnlockMutex(q->mutex);

  return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
  AVPacketList *pkt1;
  int ret;

  SDL_LockMutex(q->mutex);

  for(;;) {
    if(global_video_state->quit) {
      ret = -1;
      break;
    }

    pkt1 = q->first_pkt;
    if (pkt1) {
      q->first_pkt = pkt1->next;
      if (!q->first_pkt)
        q->last_pkt = NULL;
      q->nb_packets--;
      q->size -= pkt1->pkt.size;
      *pkt = pkt1->pkt;
      av_free(pkt1);
      ret = 1;
      break;
    } else if (!block) {
      ret = 0;
      break;
    } else {
      SDL_CondWait(q->cond, q->mutex);
    }
  }

  SDL_UnlockMutex(q->mutex);

  return ret;
}

int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size) {
  int len1, data_size = 0;
  AVPacket *pkt = &is->audio_pkt;

  for(;;) {
    while(is->audio_pkt_size > 0) {
      int got_frame = 0;
      len1 = avcodec_decode_audio4(is->audio_ctx, &is->audio_frame, &got_frame, pkt);
      if(len1 < 0) {
        /* if error, skip frame */
        is->audio_pkt_size = 0;
        break;
      }
      is->audio_pkt_data += len1;
      is->audio_pkt_size -= len1;
      data_size = 0;
      if(got_frame) {
        data_size = av_samples_get_buffer_size(NULL,
                                               is->audio_ctx->channels,
                                               is->audio_frame.nb_samples,
                                               is->audio_ctx->sample_fmt,
                                               1);
//        assert(data_size <= buf_size);
        memcpy(audio_buf, is->audio_frame.data[0], data_size);
      }
      if(data_size <= 0) {
        /* No data yet, get more frames */
        continue;
      }
      /* We have data, return it and come back for more later */
      return data_size;
    }
    if(pkt->data)
      av_free_packet(pkt);

    if(is->quit) {
      return -1;
    }

    if(packet_queue_get(&is->audioq, pkt, 1) < 0) {
      return -1;
    }
    is->audio_pkt_data = pkt->data;
    is->audio_pkt_size = pkt->size;
  }
}

void audio_callback(void *userdata, Uint8 *stream, int len) {
  VideoState *is = (VideoState *)userdata;
  int len1, audio_size;

  while(len > 0) {
    if(is->audio_buf_index >= is->audio_buf_size) {
      /* We have already sent all our data; get more */
      audio_size = audio_decode_frame(is, is->audio_buf, sizeof(is->audio_buf));
      if(audio_size < 0) {
        /* If error, output silence */
        is->audio_buf_size = 1024; // arbitrary?
        memset(is->audio_buf, 0, is->audio_buf_size);
      } else {
        is->audio_buf_size = audio_size;
      }
      is->audio_buf_index = 0;
    }
    len1 = is->audio_buf_size - is->audio_buf_index;
    if(len1 > len)
      len1 = len;
    memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
    len -= len1;
    stream += len1;
    is->audio_buf_index += len1;
  }
}

void alloc_picture(void *userdata, AVFrame *pFrame) {
  VideoState *is = (VideoState *)userdata;
  VideoPicture *vp;

  vp = &is->pictq[is->pictq_windex];
  if (vp->frame) {
    av_frame_free(&vp->frame);
  }

  SDL_LockMutex(screen_mutex);
  vp->frame = av_frame_alloc();
  vp->frame->format = pFrame->format;
  vp->frame->width = pFrame->width;
  vp->frame->height = pFrame->height;
  if (av_frame_get_buffer(vp->frame, 0) < 0) {
    fprintf(stderr, "av_frame_get_buffer failed!\n");
  }
  SDL_UnlockMutex(screen_mutex);
  vp->width = is->video_ctx->width;
  vp->height = is->video_ctx->height;
  vp->allocated = 1;
}

int queue_picture(VideoState *is, AVFrame *pFrame) {
  VideoPicture *vp;

  // wait until we have space for a new pic
  SDL_LockMutex(is->pictq_mutex);
  while (is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !is->quit) {
    SDL_CondWait(is->pictq_cond, is->pictq_mutex);
  }
  SDL_UnlockMutex(is->pictq_mutex);

  if (is->quit) {
    return -1;
  }

  // windex is set to 0 initially
  vp = &is->pictq[is->pictq_windex];

  // allocate or resize the buffer
  if (!vp->frame ||
      vp->width != is->video_st->codec->width ||
      vp->height != is->video_st->codec->height) {

    vp->allocated = 0;
    alloc_picture(is, pFrame);
    if (is->quit) {
      return -1;
    }
  }

  // We have a place to put our picture on the queue
  if (vp->frame) {
    if (av_frame_copy(vp->frame, pFrame) < 0) {
      fprintf(stderr, "av_frame_copy failed!\n");
    }
    if (++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
      is->pictq_windex = 0;
    }
    SDL_LockMutex(is->pictq_mutex);
    is->pictq_size++;
    SDL_UnlockMutex(is->pictq_mutex);
  }
  return 0;
}

int video_thread(void *arg) {
  VideoState *is = (VideoState *)arg;
  AVPacket pkt1, *packet = &pkt1;
  int frameFinished;
  AVFrame *pFrame;

  pFrame = av_frame_alloc();

  for(;;) {
    if (packet_queue_get(&is->videoq, packet, 1) < 0) {
      // means we quit getting packets
      break;
    }
    // Decode video frame
    avcodec_decode_video2(is->video_ctx, pFrame, &frameFinished, packet);

    // Did we get a video frame?
    if (frameFinished) {
      if (queue_picture(is, pFrame) < 0) {
        break;
      }
    }
    av_free_packet(packet);
  }

  av_free(pFrame);
  return 0;
}

int stream_component_open(VideoState *is, int stream_index) {
  AVFormatContext *pFormatCtx = is->pFormatCtx;
  AVCodecContext *codecCtx = NULL;
  AVCodec *codec = NULL;
  SDL_AudioSpec wanted_spec, spec;

  if(stream_index < 0 || stream_index >= pFormatCtx->nb_streams) {
    return -1;
  }

  codec = avcodec_find_decoder(pFormatCtx->streams[stream_index]->codec->codec_id);
  if(!codec) {
    fprintf(stderr, "Unsupported codec!\n");
    return -1;
  }

  codecCtx = avcodec_alloc_context3(codec);
  if(avcodec_copy_context(codecCtx, pFormatCtx->streams[stream_index]->codec) != 0) {
    fprintf(stderr, "Couldn't copy codec context");
    return -1; // Error copying codec context
  }

  if(codecCtx->codec_type == AVMEDIA_TYPE_AUDIO) {
    // Set audio settings from codec info
    wanted_spec.freq = codecCtx->sample_rate;
    wanted_spec.format = AUDIO_F32SYS;
    wanted_spec.channels = codecCtx->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = is;

    if(SDL_OpenAudio(&wanted_spec, &spec) < 0) {
      fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
      return -1;
    }
  }

  if(avcodec_open2(codecCtx, codec, NULL) < 0) {
    fprintf(stderr, "Unsupported codec!\n");
    return -1;
  }

  switch(codecCtx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
      is->audioStream = stream_index;
      is->audio_st = pFormatCtx->streams[stream_index];
      is->audio_ctx = codecCtx;
      is->audio_buf_size = 0;
      is->audio_buf_index = 0;
      memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
      packet_queue_init(&is->audioq);
      SDL_PauseAudio(0);
      break;
    case AVMEDIA_TYPE_VIDEO:
      is->videoStream = stream_index;
      is->video_st = pFormatCtx->streams[stream_index];
      is->video_ctx = codecCtx;
      packet_queue_init(&is->videoq);
      texture = SDL_CreateTexture(renderer,
                                  SDL_PIXELFORMAT_IYUV,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  codecCtx->width,
                                  codecCtx->height);
      is->video_tid = SDL_CreateThread(video_thread, "Video Thread", is);
      break;
    default:
      break;
  }
}

int decode_thread(void *arg) {
  VideoState *is = (VideoState *)arg;
  AVFormatContext  *pFormatCtx;
  AVPacket pkt1, *packet = &pkt1;

  int video_index = -1;
  int audio_index = -1;
  int i;

  is->videoStream = -1;
  is->audioStream = -1;

  global_video_state = is;

  // Open video file
  if (avformat_open_input(&pFormatCtx, is->filename, NULL, NULL) != 0) {
    return -1; // Couldn`t open file
  }
  is->pFormatCtx = pFormatCtx;

  // Retrieve stream information
  if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
    return -1; // Couldn`t find stream information
  }

  // Dump information about file onto standard error
  av_dump_format(pFormatCtx, 0, is->filename, 0);

  // Find the first video stream
  for (i = 0; i < pFormatCtx->nb_streams; ++i) {
    if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO && video_index < 0) {
      video_index = i;
    }
    if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO && audio_index < 0) {
      audio_index = i;
    }
  }
  if (audio_index >= 0) {
    stream_component_open(is, audio_index);
  }
  if (video_index >= 0) {
    stream_component_open(is, video_index);
  }

  if (is->videoStream < 0 || is->audioStream < 0) {
    fprintf(stderr, "%s: could not open codecs\n", is->filename);
    goto fail;
  }

  // main decode loop
  for (;;) {
    if (is->quit) {
      break;
    }
    // seek stuff goes here
    if (is->audioq.size > MAX_AUDIOQ_SIZE ||
        is->videoq.size > MAX_VIDEOQ_SIZE) {
      SDL_Delay(10);
      continue;
    }
    if (av_read_frame(is->pFormatCtx, packet) < 0) {
      if (is->pFormatCtx->pb->error == 0) {
        SDL_Delay(100); // no error; wait for user input
        continue;
      } else {
        break;
      }
    }
    // Is this a packet from the video stream?
    if (packet->stream_index == is->videoStream) {
      packet_queue_put(&is->videoq, packet);
    } else if (packet->stream_index == is->audioStream) {
      packet_queue_put(&is->audioq, packet);
    } else {
      av_free_packet(packet);
    }
  }
  // all done - wait for it
  while (!is->quit) {
    SDL_Delay(100);
  }

  fail:
    if (1) {
      SDL_Event event;
      event.type = FF_QUIT_EVENT;
      event.user.data1 = is;
      SDL_PushEvent(&event);
    }
  return 0;
}

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque) {
  SDL_Event event;
  event.type = FF_REFRESH_EVENT;
  event.user.data1 = opaque;
  SDL_PushEvent(&event);
  return 0; // 0 means stops timer
}

// schedule a video refresh in 'delay' ms
static void schedule_refresh(struct VideoState *is, int delay) {
  SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

void video_display(VideoState *is) {
  SDL_Rect rect;
  VideoPicture *vp;
  float aspect_ratio;
  int w, h, x, y;
  int screen_width, screen_height;
  SDL_GetWindowSize(window, &screen_width, &screen_height);

  vp = &is->pictq[is->pictq_rindex];
  if (vp->frame) {
    if (is->video_st->codec->sample_aspect_ratio.num == 0) {
      aspect_ratio = 0;
    } else {
      aspect_ratio = av_q2d(is->video_st->codec->sample_aspect_ratio) *
          is->video_st->codec->width / is->video_st->codec->height;
    }
    if (aspect_ratio <= 0.0) {
      aspect_ratio = (float) is->video_st->codec->width /
          (float) is->video_st->codec->height;
    }
    h = screen_height;
    w = ((int) rint(h * aspect_ratio)) & -3;
    if (w > screen_width) {
      w = screen_width;
      h = ((int) rint(w / aspect_ratio)) & -3;
    }
    x = (screen_width - w) / 2;
    y = (screen_height - h) / 2;

    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;

    AVFrame *pFrame = vp->frame;
    SDL_UpdateYUVTexture(texture, NULL,
                         pFrame->data[0], pFrame->linesize[0],
                         pFrame->data[1], pFrame->linesize[1],
                         pFrame->data[2], pFrame->linesize[2]);

    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, &rect);
    SDL_RenderPresent(renderer);
  }
}

void video_refresh_timer(void *userdata) {
  VideoState *is = (VideoState *)userdata;

  if (is->video_st) {
    if (is->pictq_size == 0) {
      schedule_refresh(is, 1);
    } else {
      // Timing code goes here

      schedule_refresh(is, 80);

      // show the picture
      video_display(is);

      // update queue for next picture
      if (++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
        is->pictq_rindex = 0;
      }
      SDL_LockMutex(is->pictq_mutex);
      is->pictq_size--;
      SDL_CondSignal(is->pictq_cond);
      SDL_UnlockMutex(is->pictq_mutex);
    }
  } else {
    schedule_refresh(is, 100);
  }
}

int main(int argc, char *argv[]) {
  VideoState *is;
  is = av_mallocz(sizeof(VideoState));

  av_register_all();

  // Initialize SDL
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
    fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
    exit(1);
  }

  av_strlcpy(is->filename, argv[1], sizeof(is->filename));

  is->pictq_mutex = SDL_CreateMutex();
  is->pictq_cond = SDL_CreateCond();

  window = SDL_CreateWindow("Media Player",
                            SDL_WINDOWPOS_UNDEFINED,
                            SDL_WINDOWPOS_UNDEFINED,
                            640,
                            480,
                            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  if (!window) {
    fprintf(stderr, "SDL: could not create window by SDL - exiting\n");
  }

  renderer = SDL_CreateRenderer(window, -1, 0);
  if (!renderer) {
    fprintf(stderr, "SDL: could not create Renderer by SDL - exiting\n");
  }

  schedule_refresh(is, 40);

  is->parse_tid = SDL_CreateThread(decode_thread, "Decode Thread", is);
  if(!is->parse_tid) {
    av_free(is);
    return -1;
  }

  SDL_Event event;
  for(;;) {
    SDL_WaitEvent(&event);
    switch (event.type) {
      case FF_QUIT_EVENT:
      case SDL_QUIT:
        is->quit = 1;
        SDL_Quit();
        return 0;
        break;
      case FF_REFRESH_EVENT:
        video_refresh_timer(event.user.data1);
        break;
      default:
        break;
    }
  }
  return 0;
}