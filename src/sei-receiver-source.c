/******************************************************************************
    SEI Receiver Source - Implementation
    Copyright (C) 2026

    Receives SRT streams, decodes video, extracts NTP SEI and synchronizes
******************************************************************************/

#include "sei-receiver-source.h"
#include "receiver-time.h"
#include "moblin-timecode.h"
#include "multicam-status.h"
#include <media-io/video-io.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

/* FFmpeg Headers */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

/* SRT Headers */
#ifdef _WIN32
#include <srt/srt.h>
#else
#include <srt.h>
#endif

/* Logging macros */
#define receiver_log(level, src, format, ...)                                  \
  blog(level, "[SEI Receiver: '%s'] " format,                                  \
       obs_source_get_name(src->context), ##__VA_ARGS__)

/* SRT receive buffer size */
#define SRT_BUFFER_SIZE (1024 * 1024) /* 1MB */
#define VIDEO_STALE_TIMEOUT_NS 2000000000ULL
#define PTS_BACKWARD_RESET_NS 500000000ULL
#define MAX_SYNC_PLAYOUT_DELAY_NS 2000000000ULL
#define PLAYOUT_DELAY_UPDATE_THRESHOLD_NS 25000000ULL

/*============================================================================
 * Frame buffer management
 *============================================================================*/

/* Initialise the frame buffer */
bool frame_buffer_init(frame_buffer_t *buffer) {
  if (!buffer) {
    return false;
  }

  memset(buffer, 0, sizeof(frame_buffer_t));
  if (os_sem_init(&buffer->semaphore, 1) != 0)
    return false;

  return true;
}

/* Destroy the frame buffer */
void frame_buffer_destroy(frame_buffer_t *buffer) {
  if (!buffer) {
    return;
  }

  os_sem_wait(buffer->semaphore);

  /* Free every frame's data */
  for (size_t i = 0; i < buffer->count; i++) {
    if (buffer->frames[i].data) {
      bfree(buffer->frames[i].data);
      buffer->frames[i].data = NULL;
    }
  }

  buffer->count = 0;
  buffer->read_index = 0;
  buffer->write_index = 0;

  os_sem_post(buffer->semaphore);
  os_sem_destroy(buffer->semaphore);
}

/* Add a frame to the buffer */
bool frame_buffer_push(frame_buffer_t *buffer, video_frame_data_t *frame) {
  if (!buffer || !frame) {
    return false;
  }

  os_sem_wait(buffer->semaphore);

  /* Check whether it is full */
  if (buffer->count >= MAX_FRAME_BUFFER) {
    os_sem_post(buffer->semaphore);
    return false;
  }

  /* Copy the frame data */
  size_t write_idx = buffer->write_index;
  buffer->frames[write_idx] = *frame;

  /* Copy the raw data */
  if (frame->data && frame->size > 0) {
    buffer->frames[write_idx].data = bmalloc(frame->size);
    memcpy(buffer->frames[write_idx].data, frame->data, frame->size);
  }

  /* Update the indices */
  buffer->write_index = (buffer->write_index + 1) % MAX_FRAME_BUFFER;
  buffer->count++;

  os_sem_post(buffer->semaphore);

  return true;
}

/* Take a frame from the buffer */
bool frame_buffer_pop(frame_buffer_t *buffer, video_frame_data_t *frame) {
  if (!buffer || !frame) {
    return false;
  }

  os_sem_wait(buffer->semaphore);

  /* Check whether it is empty */
  if (buffer->count == 0) {
    os_sem_post(buffer->semaphore);
    return false;
  }

  /* Take the frame */
  size_t read_idx = buffer->read_index;
  *frame = buffer->frames[read_idx];

  /* Clear the source pointer (the caller frees) */
  buffer->frames[read_idx].data = NULL;

  /* Update the indices */
  buffer->read_index = (buffer->read_index + 1) % MAX_FRAME_BUFFER;
  buffer->count--;

  os_sem_post(buffer->semaphore);

  return true;
}

/* Get the buffer size */
size_t frame_buffer_size(frame_buffer_t *buffer) {
  if (!buffer) {
    return 0;
  }

  os_sem_wait(buffer->semaphore);
  size_t count = buffer->count;
  os_sem_post(buffer->semaphore);

  return count;
}

/*============================================================================
 * Stats updates and error recovery
 *============================================================================*/

/* Forward declarations */
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts);

/* Update the live statistics */
static void update_statistics(sei_receiver_source_t *source) {
  uint64_t current_time = os_gettime_ns();

  /* Update the stats once a second */
  if (source->last_stats_update_time == 0 ||
      (current_time - source->last_stats_update_time) >= 1000000000ULL) {

    if (source->last_stats_update_time > 0) {
      /* Compute the frame rate */
      uint64_t frames_in_period =
          source->frames_rendered - source->stats_frame_count;
      double time_elapsed =
          (current_time - source->last_stats_update_time) / 1000000000.0;
      source->current_fps = (float)(frames_in_period / time_elapsed);

      /* Compute the SEI detection rate */
      if (source->frames_rendered > 0) {
        source->sei_detection_rate =
            (float)(source->sei_found_count * 100.0 / source->frames_rendered);
      }
    }

    source->last_stats_update_time = current_time;
    source->stats_frame_count = source->frames_rendered;
  }
}

/* Decoder reset (for error recovery) */
static bool reset_decoder(sei_receiver_source_t *source) {
  receiver_log(LOG_WARNING, source, "Resetting decoder due to errors...");

  /* Close the existing decoder */
  if (source->codec_context) {
    avcodec_free_context((AVCodecContext **)&source->codec_context);
    source->codec_context = NULL;
  }

  if (source->format_context && source->video_stream_index >= 0) {
    AVFormatContext *fmt_ctx = (AVFormatContext *)source->format_context;
    AVStream *vstream = fmt_ctx->streams[source->video_stream_index];

    /* Recreate the decoder */
    const AVCodec *codec = avcodec_find_decoder(vstream->codecpar->codec_id);
    if (!codec) {
      receiver_log(LOG_ERROR, source, "Failed to find decoder for reset");
      return false;
    }

    AVCodecContext *cctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(cctx, vstream->codecpar);
    cctx->pkt_timebase = vstream->time_base;

    /* Reconfigure hardware decode */
    if (source->hw_decode_enabled && source->hw_device_ctx) {
      cctx->hw_device_ctx = av_buffer_ref((AVBufferRef *)source->hw_device_ctx);
      cctx->get_format = get_hw_format;
    }

    if (avcodec_open2(cctx, codec, NULL) < 0) {
      receiver_log(LOG_ERROR, source, "Failed to reopen codec");
      avcodec_free_context(&cctx);
      return false;
    }

    source->codec_context = cctx;
    source->decode_error_count = 0;
    receiver_log(LOG_INFO, source, "Decoder reset successful");
    return true;
  }

  return false;
}

/*============================================================================
 * Hardware decode support
 *============================================================================*/

/* Hardware pixel format callback */
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts) {
  const enum AVPixelFormat *p;

  /* Prefer the hardware format */
  for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
    switch (*p) {
    case AV_PIX_FMT_QSV:
    case AV_PIX_FMT_CUDA:
    case AV_PIX_FMT_D3D11:
      return *p;
    default:
      break;
    }
  }
  return AV_PIX_FMT_NONE;
}

/* Initialise the hardware device context */
static bool init_hw_device(sei_receiver_source_t *source) {
  if (!source || !source->hw_decode_enabled)
    return false;

  enum AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_NONE;

  /* Pick the hardware type from the configuration */
  if (strcmp(source->hw_decoder_type, "qsv") == 0) {
    hw_type = AV_HWDEVICE_TYPE_QSV;
  } else if (strcmp(source->hw_decoder_type, "nvdec") == 0) {
    hw_type = AV_HWDEVICE_TYPE_CUDA;
  } else if (strcmp(source->hw_decoder_type, "amf") == 0) {
    hw_type = AV_HWDEVICE_TYPE_D3D11VA;
  }

  if (hw_type == AV_HWDEVICE_TYPE_NONE)
    return false;

  AVBufferRef *hw_device_ctx = NULL;
  int ret = av_hwdevice_ctx_create(&hw_device_ctx, hw_type, NULL, NULL, 0);
  if (ret < 0) {
    receiver_log(LOG_WARNING, source,
                 "Failed to create HW device (%s), falling back to SW decode",
                 source->hw_decoder_type);
    source->hw_decode_enabled = false;
    return false;
  }

  source->hw_device_ctx = hw_device_ctx;
  receiver_log(LOG_INFO, source, "Hardware decoder initialized: %s",
               source->hw_decoder_type);
  return true;
}

/*============================================================================
 * Video decode and SEI extraction
 *============================================================================*/

/* Decode and extract the SEI */
static void record_decode_error(sei_receiver_source_t *source)
{
  if (!source)
    return;
  source->decode_errors_total++;
  multicam_status_set_decode_errors(source, source->decode_errors_total);
}

static void record_audio_decode_error(sei_receiver_source_t *source)
{
  if (!source)
    return;
  source->audio_decode_errors_total++;
  multicam_status_set_audio_decode_errors(source,
                                          source->audio_decode_errors_total);
}

static void clear_output_video(sei_receiver_source_t *source)
{
  if (!source || !source->context || source->video_output_cleared)
    return;
  obs_source_output_video(source->context, NULL);
  source->video_output_cleared = true;
  receiver_log(LOG_INFO, source,
               "Cleared stale video output after 2 seconds without decoded frames");
}

static int interrupt_if_video_stale(void *opaque)
{
  sei_receiver_source_t *source = (sei_receiver_source_t *)opaque;
  if (source) {
    bool reset_requested;
    pthread_mutex_lock(&source->audio_settings_lock);
    reset_requested = source->reset_picture_requested;
    pthread_mutex_unlock(&source->audio_settings_lock);
    if (reset_requested)
      return 1;
  }
  if (!source || !source->thread_active || !source->last_video_output_mono_ns ||
      source->video_output_cleared)
    return 0;
  uint64_t now = os_gettime_ns();
  if (receiver_video_is_stale(source->last_video_output_mono_ns, now,
                              VIDEO_STALE_TIMEOUT_NS)) {
    clear_output_video(source);
    return 1;
  }
  return 0;
}

typedef struct playout_filter_update {
  obs_source_t *filter;
  uint32_t delay_ms;
} playout_filter_update_t;

static void apply_playout_filter_update(void *opaque)
{
  playout_filter_update_t *update = (playout_filter_update_t *)opaque;
  if (!update) return;
  obs_data_t *settings = obs_data_create();
  obs_data_set_int(settings, "delay_ms", update->delay_ms);
  obs_source_update(update->filter, settings);
  obs_data_release(settings);
  obs_source_release(update->filter);
  bfree(update);
}

static void schedule_playout_delay(sei_receiver_source_t *source,
                                  uint64_t delay_ns, bool force)
{
  if (!source) return;
  if (delay_ns > MAX_SYNC_PLAYOUT_DELAY_NS) {
    if (!source->playout_delay_cap_warning_logged) {
      source->playout_delay_cap_warning_logged = true;
      receiver_log(LOG_WARNING, source,
                   "Required playout delay exceeds the 2000 ms safety cap; output may remain misaligned");
    }
    delay_ns = MAX_SYNC_PLAYOUT_DELAY_NS;
  }
  uint64_t difference = delay_ns > source->playout_delay_ns
                            ? delay_ns - source->playout_delay_ns
                            : source->playout_delay_ns - delay_ns;
  if (!force && difference < PLAYOUT_DELAY_UPDATE_THRESHOLD_NS) return;

  if (!source->playout_delay_filter) {
    source->playout_delay_ns = 0;
    multicam_status_set_playout_delay(source, 0);
    if (!source->playout_filter_warning_logged) {
      source->playout_filter_warning_logged = true;
      receiver_log(LOG_WARNING, source,
                   "OBS async-delay filter unavailable; cannot align this receiver's playout");
    }
    return;
  }
  playout_filter_update_t *update = bzalloc(sizeof(*update));
  if (!update) return;
  update->filter = source->playout_delay_filter;
  update->delay_ms = (uint32_t)((delay_ns + 500000ULL) / 1000000ULL);
  uint32_t delay_ms = update->delay_ms;
  obs_source_addref(update->filter);
  obs_queue_task(OBS_TASK_UI, apply_playout_filter_update, update, false);
  source->playout_delay_ns = delay_ns;
  multicam_status_set_playout_delay(source, delay_ns);
  receiver_log(LOG_INFO, source,
               "Scheduling OBS playout delay update: %u ms (group maximum input age)",
               delay_ms);
}

static void clear_if_video_stale(sei_receiver_source_t *source)
{
  if (!source || source->video_output_cleared ||
      !source->last_video_output_mono_ns)
    return;
  if (receiver_video_is_stale(source->last_video_output_mono_ns,
                              os_gettime_ns(), VIDEO_STALE_TIMEOUT_NS))
    clear_output_video(source);
}

bool decode_and_extract_sei(sei_receiver_source_t *source, AVPacket *packet,
                            video_frame_data_t *frame_out) {
  if (!source || !packet || !frame_out) {
    return false;
  }

  AVCodecContext *codec_ctx = (AVCodecContext *)source->codec_context;
  if (!codec_ctx) {
    return false;
  }

  /* Send the packet to the decoder */
  static uint64_t send_count = 0;
  send_count++;

  int ret = avcodec_send_packet(codec_ctx, packet);
  if (ret < 0) {
    receiver_log(LOG_ERROR, source, "Failed to send packet to decoder: %d (%s)",
                 ret, av_err2str(ret));
    record_decode_error(source);
    return false;
  }

  if (send_count % 30 == 1) {
    receiver_log(LOG_INFO, source, "Packet sent to decoder: #%llu", send_count);
  }

  /* Receive the decoded frame */
  AVFrame *av_frame = av_frame_alloc();
  if (!av_frame) {
    record_decode_error(source);
    return false;
  }

  ret = avcodec_receive_frame(codec_ctx, av_frame);
  if (ret < 0) {
    av_frame_free(&av_frame);

    /* EAGAIN means the decoder wants more input, which is normal */
    if (ret == AVERROR(EAGAIN)) {
      static uint64_t eagain_count = 0;
      eagain_count++;
      if (eagain_count % 100 == 1) {
        receiver_log(LOG_DEBUG, source,
                     "Decoder needs more data (EAGAIN count: %llu)",
                     eagain_count);
      }
      return true; /* packet consumed, no frame out yet */
    }

    /* EOF is a normal end too */
    if (ret == AVERROR_EOF) {
      receiver_log(LOG_INFO, source, "Decoder EOF");
      return false;
    }

    /* anything else is a real problem */
    receiver_log(LOG_ERROR, source, "Video decode failed: %d (%s)", ret,
                 av_err2str(ret));

    /* Error recovery: bump the error count */
    source->decode_error_count++;
    record_decode_error(source);
    if (source->decode_error_count >= source->decode_error_threshold) {
      receiver_log(LOG_WARNING, source,
                   "Decoder error threshold reached (%u), attempting reset",
                   source->decode_error_count);
      reset_decoder(source);
    }
    return false;
  }

  static uint64_t video_frame_count = 0;
  video_frame_count++;
  if (video_frame_count % 30 == 1) {
    receiver_log(
        LOG_INFO, source, "Video frame decoded: #%llu, format=%d, size=%dx%d",
        video_frame_count, av_frame->format, av_frame->width, av_frame->height);
  }

  /* Decode succeeded; reset the error count */
  source->decode_error_count = 0;

  /* Hardware frames: copy to system memory */
  if (av_frame->format == AV_PIX_FMT_QSV ||
      av_frame->format == AV_PIX_FMT_CUDA ||
      av_frame->format == AV_PIX_FMT_D3D11) {
    AVFrame *sw_frame = av_frame_alloc();
    if (!sw_frame) {
      av_frame_free(&av_frame);
      return false;
    }

    /* hardware frame -> system memory */
    ret = av_hwframe_transfer_data(sw_frame, av_frame, 0);
    if (ret < 0) {
      receiver_log(LOG_ERROR, source, "Failed to transfer HW frame to SW: %d",
                   ret);
      av_frame_free(&sw_frame);
      av_frame_free(&av_frame);
      return false;
    }

    sw_frame->pts = av_frame->pts;
    sw_frame->best_effort_timestamp = av_frame->best_effort_timestamp;
    av_frame_free(&av_frame);
    av_frame = sw_frame;
  }

  /* AVFrame PTS retains the originating stream's time base. */
  AVFormatContext *format_ctx = (AVFormatContext *)source->format_context;
  AVRational video_time_base =
      format_ctx->streams[source->video_stream_index]->time_base;
  int64_t video_pts = av_frame->best_effort_timestamp;
  if (video_pts == AV_NOPTS_VALUE)
    video_pts = av_frame->pts;
  if (video_pts == AV_NOPTS_VALUE)
    video_pts = packet->pts;
  int64_t pts_ns = video_pts;
  if (pts_ns == AV_NOPTS_VALUE)
    pts_ns = os_gettime_ns();
  else
    pts_ns = receiver_pts_to_ns(pts_ns, video_time_base.num,
                                video_time_base.den);

  /* A sender can switch to a new PTS origin without dropping the SRT link.
   * Never apply the old camera's PTS-to-OBS mapping after a large rewind. */
  if (source->last_video_pts_ns != AV_NOPTS_VALUE &&
      receiver_pts_has_backward_reset(source->last_video_pts_ns, pts_ns,
                                      PTS_BACKWARD_RESET_NS)) {
    receiver_log(LOG_WARNING, source,
                 "Video PTS moved backwards; resetting sync anchor");
    source->has_pts_offset = false;
    source->pts_offset = 0;
    source->sync_method = SYNC_METHOD_UNKNOWN;
    source->absolute_sync_method = SYNC_METHOD_UNKNOWN;
  }
  source->last_video_pts_ns = pts_ns;

  /* Fill in the frame info */
  frame_out->width = av_frame->width;
  frame_out->height = av_frame->height;
  frame_out->pts = pts_ns;
  frame_out->format = VIDEO_FORMAT_I420; /* YUV420P by default */

  /* Compute the frame size and allocate (align 32 for OBS) */
  /* BGRA Output */
  int frame_size = av_image_get_buffer_size(AV_PIX_FMT_BGRA, av_frame->width,
                                            av_frame->height, 32);
  frame_out->data = bmalloc(frame_size);
  frame_out->size = frame_size;

  /* Format conversion (SwsScale to BGRA) */
  if (!source->sws_ctx || av_frame->width != source->width ||
      av_frame->height != source->height ||
      av_frame->format != ((AVCodecContext *)source->codec_context)->pix_fmt) {

    if (source->sws_ctx)
      sws_freeContext(source->sws_ctx);

    source->sws_ctx = sws_getContext(
        av_frame->width, av_frame->height, av_frame->format, av_frame->width,
        av_frame->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL, NULL, NULL);

    source->width = av_frame->width;
    source->height = av_frame->height;
    ((AVCodecContext *)source->codec_context)->pix_fmt = av_frame->format;

    if (!source->sws_ctx) {
      receiver_log(LOG_ERROR, source, "Failed to initialize SwsContext");
      av_frame_free(&av_frame);
      bfree(frame_out->data);
      frame_out->data = NULL;
      return false;
    }
  }

  /* Copy the image data */
  uint8_t *dest[4] = {0};
  int linesize[4] = {0};

  av_image_fill_arrays(dest, linesize, frame_out->data, AV_PIX_FMT_BGRA,
                       av_frame->width, av_frame->height, 32);

  sws_scale(source->sws_ctx, (const uint8_t *const *)av_frame->data,
            av_frame->linesize, 0, av_frame->height, dest, linesize);

  /* Try to pull the SEI out of the side data */
  frame_out->has_ntp = false;
  frame_out->timestamp_source = SYNC_METHOD_UNKNOWN;

  AVFrameSideData *sei_data =
      av_frame_get_side_data(av_frame, AV_FRAME_DATA_SEI_UNREGISTERED);

  if (sei_data) {
    /* Parse the NTP SEI */
    ntp_sei_data_t ntp_data;
    if (parse_ntp_sei(sei_data->data, sei_data->size, &ntp_data)) {
      frame_out->ntp_time = ntp_data.ntp_time;
      frame_out->has_ntp = true;
      frame_out->timestamp_source = SYNC_METHOD_SEI_STAMPER_NTP;
      source->sei_found_count++;

      receiver_log(LOG_DEBUG, source,
                   "Extracted NTP SEI: seconds=%u, fraction=%u",
                   ntp_data.ntp_time.seconds, ntp_data.ntp_time.fraction);
    }
  }
  if (!frame_out->has_ntp) {
    /* FFmpeg does not expose HEVC time_code as unregistered user data. */
    const uint8_t *payload = NULL;
    size_t payload_size = 0;

    if (extract_sei_payload(packet->data, packet->size, &payload,
                            &payload_size)) {
      ntp_sei_data_t ntp_data;
      if (parse_ntp_sei(payload, payload_size, &ntp_data)) {
        frame_out->ntp_time = ntp_data.ntp_time;
        frame_out->has_ntp = true;
        frame_out->timestamp_source = SYNC_METHOD_SEI_STAMPER_NTP;
        source->sei_found_count++;
      }
    }
  }

  if (!frame_out->has_ntp && source->ntp_enabled) {
    AVFormatContext *format_ctx = (AVFormatContext *)source->format_context;
    moblin_timecode_t moblin_tc;
    if (!frame_out->has_ntp && source->ntp_enabled && format_ctx &&
        source->video_stream_index >= 0 &&
        codec_ctx->codec_id == AV_CODEC_ID_HEVC &&
        moblin_extract_timecode_annexb(packet->data, packet->size,
                                       &moblin_tc)) {
      ntp_timestamp_t current_ntp;
      int64_t current_unix_ns, mapped_unix_ns;
      AVStream *stream = format_ctx->streams[source->video_stream_index];
      AVRational fps = {0, 1};
      moblin_select_timecode_rate(stream->r_frame_rate.num,
                                  stream->r_frame_rate.den,
                                  stream->avg_frame_rate.num,
                                  stream->avg_frame_rate.den, &fps.num,
                                  &fps.den);
      /* A fresh local NTP anchor supplies the missing calendar date. With no
       * trustworthy anchor or frame rate, retain the explicit PTS fallback. */
      uint32_t ntp_max_age =
          receiver_ntp_max_age_seconds(source->ntp_sync_interval_ms);
      if (!ntp_client_needs_resync(&source->ntp_client, ntp_max_age) &&
          ntp_client_get_time(&source->ntp_client, &current_ntp) &&
          receiver_ntp_to_unix_ns(current_ntp.seconds, current_ntp.fraction,
                                  &current_unix_ns) &&
          moblin_timecode_resolve_unix_ns(&moblin_tc, current_unix_ns,
                                          fps.num, fps.den,
                                          &mapped_unix_ns)) {
        uint64_t unix_seconds = (uint64_t)(mapped_unix_ns / 1000000000LL);
        uint64_t fraction_ns = (uint64_t)(mapped_unix_ns % 1000000000LL);
        frame_out->ntp_time.seconds =
            (uint32_t)(unix_seconds + 2208988800ULL);
        frame_out->ntp_time.fraction =
            (uint32_t)((fraction_ns << 32) / 1000000000ULL);
        frame_out->has_ntp = true;
        frame_out->timestamp_source = SYNC_METHOD_MOBLIN_TIMECODE;
        source->sync_method = SYNC_METHOD_MOBLIN_TIMECODE;
        source->sei_found_count++;
      }
    }
  }

  /* Output to OBS */
  struct obs_source_frame obs_frame = {0};

  obs_frame.data[0] = dest[0];
  obs_frame.linesize[0] = linesize[0];

  obs_frame.width = frame_out->width;
  obs_frame.height = frame_out->height;
  obs_frame.format = VIDEO_FORMAT_BGRA;

  /* Calculate correct Sync Timestamp */
  if (frame_out->has_ntp)
    source->sync_method = frame_out->timestamp_source;
  obs_frame.timestamp = calculate_display_time(source, frame_out);

  int64_t absolute_unix_ns = 0;
  int64_t current_unix_ns = 0;
  uint64_t observed_latency_ns = 0;
  uint64_t status_now_ns = os_gettime_ns();
  bool ntp_fresh = false;
  multicam_status_sync_method_t status_method = MULTICAM_SYNC_PTS_FALLBACK;
  if (source->sync_method == SYNC_METHOD_SEI_STAMPER_NTP)
    status_method = MULTICAM_SYNC_SEI_STAMPER_NTP;
  else if (source->sync_method == SYNC_METHOD_MOBLIN_TIMECODE)
    status_method = MULTICAM_SYNC_MOBLIN_TIMECODE;
  if (status_method == MULTICAM_SYNC_SEI_STAMPER_NTP ||
      status_method == MULTICAM_SYNC_MOBLIN_TIMECODE) {
    ntp_fresh = !ntp_client_needs_resync(
        &source->ntp_client,
        receiver_ntp_max_age_seconds(source->ntp_sync_interval_ms));
    if (ntp_fresh) {
      ntp_timestamp_t current_ntp;
      bool have_current_time =
          ntp_client_get_time(&source->ntp_client, &current_ntp) &&
          receiver_ntp_to_unix_ns(current_ntp.seconds, current_ntp.fraction,
                                  &current_unix_ns);
      if (frame_out->has_ntp &&
          receiver_ntp_to_unix_ns(frame_out->ntp_time.seconds,
                                  frame_out->ntp_time.fraction,
                                  &absolute_unix_ns)) {
        /* Current frame has an absolute timestamp. */
      } else {
        if (have_current_time &&
            receiver_monotonic_to_unix_ns((int64_t)obs_frame.timestamp,
                                          (int64_t)status_now_ns,
                                          current_unix_ns,
                                          &absolute_unix_ns)) {
          /* Project this PTS-only frame through the fresh NTP anchor. */
        } else {
          ntp_fresh = false;
        }
      }
      if (ntp_fresh && have_current_time && absolute_unix_ns > 0 &&
          current_unix_ns > absolute_unix_ns)
        observed_latency_ns = (uint64_t)(current_unix_ns - absolute_unix_ns);
    }
  }
  multicam_status_video(source, status_method, ntp_fresh, absolute_unix_ns,
                        observed_latency_ns,
                        frame_out->width, frame_out->height, source->pts_offset,
                        status_now_ns);
  if (ntp_fresh && absolute_unix_ns > 0) {
    if (!source->smoothed_input_latency_ns) {
      source->smoothed_input_latency_ns = observed_latency_ns;
    } else if (observed_latency_ns >= source->smoothed_input_latency_ns) {
      source->smoothed_input_latency_ns +=
          (observed_latency_ns - source->smoothed_input_latency_ns) / 8;
    } else {
      source->smoothed_input_latency_ns -=
          (source->smoothed_input_latency_ns - observed_latency_ns) / 8;
    }
    multicam_status_set_sync_latency(source,
                                     source->smoothed_input_latency_ns);
    uint64_t group_max_latency =
        multicam_status_group_max_latency_ns(status_now_ns);
    schedule_playout_delay(
        source, multicam_status_playout_delay(group_max_latency,
                                              source->smoothed_input_latency_ns),
        false);
  }

  /* Debug log: confirm the timestamp */
  receiver_log(
      LOG_DEBUG, source, "Video Decoded: %dx%d, PTS_IN=%lld, TS_OUT=%lld",
      obs_frame.width, obs_frame.height, packet->pts, obs_frame.timestamp);

  /* Push the frame to OBS */
  obs_source_output_video(source->context, &obs_frame);
  source->last_video_output_mono_ns = os_gettime_ns();
  source->video_output_cleared = false;

  /* Update the statistics */
  update_statistics(source);

  /* Free the AVFrame */
  av_frame_free(&av_frame);

  /*
   * OBS outputs frames synchronously (copying data) by default
   * So we should free our buffer here.
   */
  if (frame_out->data) {
    bfree(frame_out->data);
    frame_out->data = NULL;
  }

  return true;
}

/* Synchronised timestamp (PTS nanoseconds -> OBS monotonic nanoseconds) */
int64_t get_sync_timestamp(sei_receiver_source_t *source, int64_t pts_ns) {
  if (!source)
    return 0;

  int64_t current_time = os_gettime_ns();

  /* All callers pass timestamps converted to nanoseconds. */

  if (!source->has_pts_offset) {
    source->pts_offset = current_time - pts_ns;
    source->has_pts_offset = true;

    receiver_log(LOG_INFO, source,
                 "Initialized Sync Offset: PTS=%lld, Local=%lld, Offset=%lld",
                 pts_ns, current_time, source->pts_offset);
  }

  return pts_ns + source->pts_offset;
}

/* Compute the video presentation time */
int64_t calculate_display_time(sei_receiver_source_t *source,
                               video_frame_data_t *frame) {
  if (!source || !frame) {
    return 0;
  }

  /* With an NTP timestamp, and NTP sync enabled */
  if (frame->has_ntp && source->ntp_enabled &&
      !ntp_client_needs_resync(
          &source->ntp_client,
          receiver_ntp_max_age_seconds(source->ntp_sync_interval_ms))) {
    /* Nanoseconds for the NTP timestamp */
    /* Convert the absolute NTP stamp into OBS's monotonic timestamp domain
     * using the NTP client's current server-time/monotonic-time anchor. */
    ntp_timestamp_t current_ntp;
    int64_t frame_ntp_ns, current_ntp_ns;
    if (frame->ntp_time.seconds < 3786825600U ||
        !ntp_client_get_time(&source->ntp_client, &current_ntp) ||
        !receiver_ntp_to_unix_ns(frame->ntp_time.seconds,
                                 frame->ntp_time.fraction, &frame_ntp_ns) ||
        !receiver_ntp_to_unix_ns(current_ntp.seconds, current_ntp.fraction,
                                 &current_ntp_ns) ||
        frame_ntp_ns - current_ntp_ns > 86400000000000LL ||
        current_ntp_ns - frame_ntp_ns > 86400000000000LL) {
      source->sync_method = SYNC_METHOD_PTS_FALLBACK;
      return get_sync_timestamp(source, frame->pts);
    }
    int64_t display_time;
    if (!receiver_ntp_to_monotonic_ns(
            frame->ntp_time.seconds, frame->ntp_time.fraction,
            current_ntp.seconds, current_ntp.fraction,
            (int64_t)os_gettime_ns(), &display_time)) {
      source->sync_method = SYNC_METHOD_PTS_FALLBACK;
      return get_sync_timestamp(source, frame->pts);
    }

    /* Log periodically only, to avoid flooding */
    if (source->sei_found_count % 300 == 0) {
      receiver_log(LOG_DEBUG, source,
                   "Absolute Sync: NTP=%lld, LocalAnchor=%lld, Display=%lld",
                   frame_ntp_ns, current_ntp_ns, display_time);
    }

    /* Update the shared PTS offset so audio follows the video's pacing */
    /* this matters because audio may still be using PTS */
    /* AudioTarget = VideoDisplayTime */
    /* VideoPTS + PTSOffset = VideoDisplayTime */
    /* PTSOffset = VideoDisplayTime - VideoPTS */

    /* Update the PTS offset on every SEI (keyframe/IDR) */
    // if (source->sei_found_count % 1 == 0) // Always
    source->pts_offset = display_time - frame->pts;
    source->has_pts_offset = true;
    source->absolute_sync_method = frame->timestamp_source;
    if (source->absolute_sync_method != SYNC_METHOD_MOBLIN_TIMECODE)
      source->absolute_sync_method = SYNC_METHOD_SEI_STAMPER_NTP;
    source->sync_method = source->absolute_sync_method;

    return display_time;
  }

  bool ntp_fresh = source->ntp_enabled &&
      !ntp_client_needs_resync(&source->ntp_client,
          receiver_ntp_max_age_seconds(source->ntp_sync_interval_ms));
  if (receiver_keep_absolute_sync(
          source->absolute_sync_method != SYNC_METHOD_UNKNOWN,
          source->ntp_enabled, ntp_fresh) && source->has_pts_offset) {
    source->sync_method = source->absolute_sync_method;
    return get_sync_timestamp(source, frame->pts);
  }
  source->sync_method = SYNC_METHOD_PTS_FALLBACK;
  source->absolute_sync_method = SYNC_METHOD_UNKNOWN;
  return get_sync_timestamp(source, frame->pts);
}

/* Decode audio */
bool decode_audio(sei_receiver_source_t *source, AVPacket *packet) {
  if (!source || !packet || !source->audio_codec_context)
    return false;

  AVCodecContext *ctx = (AVCodecContext *)source->audio_codec_context;
  int ret = avcodec_send_packet(ctx, packet);
  if (ret < 0) {
    receiver_log(LOG_ERROR, source, "Error sending audio packet: %d", ret);
    record_audio_decode_error(source);
    return false;
  }

  AVFrame *frame = av_frame_alloc();
  if (!frame)
  {
    record_audio_decode_error(source);
    return false;
  }

  bool success = true;
  while (ret >= 0) {
    ret = avcodec_receive_frame(ctx, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      break;
    } else if (ret < 0) {
      receiver_log(LOG_ERROR, source, "Error receiving audio frame: %d", ret);
      record_audio_decode_error(source);
      success = false;
      break;
    }

    struct obs_source_audio audio = {0};
    int channels = frame->ch_layout.nb_channels;
    if (channels <= 0 || channels > MAX_AUDIO_CHANNELS || frame->sample_rate <= 0) {
      receiver_log(LOG_WARNING, source, "Unsupported decoded audio layout/rate");
      av_frame_unref(frame);
      continue;
    }

    /* The decoder can return packed or planar samples in multiple formats.
     * Always convert into the declared OBS format (float planar) so the data
     * layout and sample type agree with audio.format. */
    SwrContext *resampler = (SwrContext *)source->audio_resampler;
    if (resampler &&
        (source->audio_resampler_sample_fmt != frame->format ||
         source->audio_resampler_sample_rate != frame->sample_rate ||
         av_channel_layout_compare(&source->audio_resampler_layout,
                                   &frame->ch_layout) != 0)) {
      swr_free(&resampler);
      source->audio_resampler = NULL;
      av_channel_layout_uninit(&source->audio_resampler_layout);
      resampler = NULL;
    }
    if (!resampler) {
      AVChannelLayout output_layout = frame->ch_layout;
      ret = swr_alloc_set_opts2(&resampler, &output_layout, AV_SAMPLE_FMT_FLTP,
                                frame->sample_rate, &frame->ch_layout,
                                (enum AVSampleFormat)frame->format,
                                frame->sample_rate, 0, NULL);
      if (ret >= 0)
        ret = swr_init(resampler);
      if (ret < 0) {
        receiver_log(LOG_ERROR, source, "Failed to initialize audio conversion: %d", ret);
        record_audio_decode_error(source);
        swr_free(&resampler);
        source->audio_resampler = NULL;
        success = false;
        av_frame_unref(frame);
        break;
      }
      source->audio_resampler = resampler;
      source->audio_resampler_sample_fmt = frame->format;
      source->audio_resampler_sample_rate = frame->sample_rate;
      ret = av_channel_layout_copy(&source->audio_resampler_layout,
                                   &frame->ch_layout);
      if (ret < 0) {
        swr_free(&resampler);
        source->audio_resampler = NULL;
        success = false;
        av_frame_unref(frame);
        break;
      }
    }

    uint8_t **converted = NULL;
    int converted_line_size = 0;
    ret = av_samples_alloc_array_and_samples(&converted, &converted_line_size,
                                              channels, frame->nb_samples,
                                              AV_SAMPLE_FMT_FLTP, 0);
    if (ret < 0) {
      receiver_log(LOG_ERROR, source, "Failed to allocate converted audio: %d", ret);
      record_audio_decode_error(source);
      success = false;
      av_frame_unref(frame);
      break;
    }
    int converted_samples = swr_convert(resampler, converted, frame->nb_samples,
                                         (const uint8_t **)frame->extended_data,
                                         frame->nb_samples);
    if (converted_samples < 0) {
      receiver_log(LOG_ERROR, source, "Audio sample conversion failed: %d", converted_samples);
      record_audio_decode_error(source);
      av_freep(&converted[0]);
      av_freep(&converted);
      success = false;
      av_frame_unref(frame);
      break;
    }

    for (int i = 0; i < channels; i++)
      audio.data[i] = converted[i];
    audio.frames = (uint32_t)converted_samples;

    if (channels == 1)
      audio.speakers = SPEAKERS_MONO;
    else if (channels == 2)
      audio.speakers = SPEAKERS_STEREO;
    else if (channels == 4)
      audio.speakers = SPEAKERS_4POINT0;
    else if (channels == 6)
      audio.speakers = SPEAKERS_5POINT1;
    else
      audio.speakers = SPEAKERS_UNKNOWN;

    audio.format = source->audio_format;
    audio.samples_per_sec = (uint32_t)frame->sample_rate;

    /* Timestamp sync */
    /* Get the audio PTS (timebase-based) */
    int64_t pts = frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE)
      pts = frame->pts;
    if (pts == AV_NOPTS_VALUE)
      pts = packet->pts;
    int64_t pts_ns = 0;

    if (pts != AV_NOPTS_VALUE) {
      AVFormatContext *format_ctx = (AVFormatContext *)source->format_context;
      AVRational tb = format_ctx->streams[source->audio_stream_index]->time_base;
      if (tb.num <= 0 || tb.den <= 0)
        tb = (AVRational){1, frame->sample_rate};
      pts_ns = receiver_pts_to_ns(pts, tb.num, tb.den);
    } else {
      /* Keep un-timestamped audio on the established local clock. */
      pts_ns = source->has_pts_offset ? (int64_t)os_gettime_ns() - source->pts_offset :
                                      (int64_t)os_gettime_ns();
    }

    /* Use the shared timestamp sync */
    pthread_mutex_lock(&source->audio_settings_lock);
    int64_t audio_offset_ns = source->audio_offset_ns;
    pthread_mutex_unlock(&source->audio_settings_lock);
    audio.timestamp = receiver_audio_timestamp_with_offset(
        get_sync_timestamp(source, pts_ns), audio_offset_ns);

    obs_source_output_audio(source->context, &audio);
    multicam_status_audio(source, os_gettime_ns());
    av_freep(&converted[0]);
    av_freep(&converted);
    av_frame_unref(frame);
  }

  av_frame_free(&frame);
  return success;
}

/*============================================================================
 * SRT receive thread
 *============================================================================*/

/* SRT receive thread */

/*============================================================================
 * OBS source callbacks
 *============================================================================*/

/* Source name */
static const char *receiver_source_getname(void *unused) {
  UNUSED_PARAMETER(unused);
  return obs_module_text("SEIReceiver");
}

static void start_receiver(void *data);
static void stop_receiver(void *data);

/* Create the source */
static void *receiver_source_create(obs_data_t *settings,
                                    obs_source_t *source) {
  sei_receiver_source_t *ctx = bzalloc(sizeof(sei_receiver_source_t));
  ctx->context = source;
  ctx->reset_sync_requested = false;
  ctx->reset_picture_requested = false;
  ctx->last_video_pts_ns = AV_NOPTS_VALUE;
  ctx->video_output_cleared = true;

  /* Initialise the frame buffer */
  if (!frame_buffer_init(&ctx->frame_buffer)) {
    bfree(ctx);
    return NULL;
  }
  if (pthread_mutex_init(&ctx->audio_settings_lock, NULL) != 0) {
    frame_buffer_destroy(&ctx->frame_buffer);
    bfree(ctx);
    return NULL;
  }
  ctx->audio_settings_lock_initialized = true;
  multicam_status_register(ctx, obs_source_get_name(source));

  /* OBS timestamps old async frames as late and caps its queued frames at 30.
   * Keep a real bounded playout delay in OBS's stock filter so faster inputs
   * can wait for a slower Moblin path without falsifying their timestamps. */
  obs_data_t *delay_settings = obs_data_create();
  obs_data_set_int(delay_settings, "delay_ms", 0);
  ctx->playout_delay_filter = obs_source_create_private(
      "async_delay_filter", "SEI Sync Playout Delay", delay_settings);
  obs_data_release(delay_settings);
  if (ctx->playout_delay_filter)
    obs_source_filter_add(ctx->context, ctx->playout_delay_filter);
  else
    receiver_log(LOG_WARNING, ctx,
                 "OBS async-delay filter was not available; adaptive playout alignment is disabled");

  /* Initialise the sync state */
  ctx->sync_state = SYNC_STATE_WAITING;
  ctx->has_pts_offset = false;
  ctx->pts_offset = 0;

  /* Load the configuration from settings */
  const char *srt_url = obs_data_get_string(settings, "srt_url");
  if (srt_url && srt_url[0]) {
    strncpy(ctx->srt_url, srt_url, sizeof(ctx->srt_url) - 1);
  }

  /* Hardware decoder settings */
  const char *hw_decoder = obs_data_get_string(settings, "hw_decoder");
  if (hw_decoder && hw_decoder[0]) {
    strncpy(ctx->hw_decoder_type, hw_decoder, sizeof(ctx->hw_decoder_type) - 1);
    ctx->hw_decode_enabled = (strcmp(hw_decoder, "none") != 0);
  } else {
    strcpy(ctx->hw_decoder_type, "none");
    ctx->hw_decode_enabled = false;
  }

  /* Codec format settings (a manual choice wins) */
  const char *codec_format = obs_data_get_string(settings, "codec_format");
  if (codec_format && codec_format[0]) {
    strncpy(ctx->codec_format, codec_format, sizeof(ctx->codec_format) - 1);
  } else {
    strcpy(ctx->codec_format, "auto"); // auto-detect by default
  }

  const char *ntp_server = obs_data_get_string(settings, "ntp_server");
  if (ntp_server && ntp_server[0]) {
    strncpy(ctx->ntp_server, ntp_server, sizeof(ctx->ntp_server) - 1);
  } else {
    strcpy(ctx->ntp_server, "time.windows.com");
  }

  ctx->ntp_port = (uint16_t)obs_data_get_int(settings, "ntp_port");
  if (ctx->ntp_port == 0) {
    ctx->ntp_port = 123;
  }

  ctx->ntp_enabled = obs_data_get_bool(settings, "ntp_enabled");
  ctx->ntp_drift_threshold_ms =
      (uint32_t)obs_data_get_int(settings, "ntp_drift_threshold");
  if (ctx->ntp_drift_threshold_ms == 0) {
    ctx->ntp_drift_threshold_ms = 50; // default 50 ms
  }

  ctx->ntp_sync_interval_ms =
      (uint32_t)obs_data_get_int(settings, "ntp_sync_interval");
  if (ctx->ntp_sync_interval_ms == 0) {
    ctx->ntp_sync_interval_ms = 10000; // default 10 s, to be safe
  }
  ctx->audio_offset_ns =
      (int64_t)obs_data_get_int(settings, "audio_offset_ms") * 1000000LL;
  multicam_status_set_audio_offset(
      ctx, (int32_t)(ctx->audio_offset_ns / 1000000LL));

  /* Initialise statistics and error recovery */
  ctx->last_stats_update_time = 0;
  ctx->stats_frame_count = 0;
  ctx->current_fps = 0.0f;
  ctx->sei_detection_rate = 0.0f;
  ctx->decode_error_count = 0;
  ctx->decode_error_threshold = 10; /* reset after 10 consecutive errors */

  /* Initialise the NTP client */
  if (ctx->ntp_enabled) {
    if (ntp_client_init(&ctx->ntp_client, ctx->ntp_server, ctx->ntp_port)) {
      receiver_log(LOG_INFO, ctx, "NTP client initialized");
      /* Resolve DNS and sync only on the NTP worker, never in OBS or decode. */
      ntp_client_start_background_sync(&ctx->ntp_client,
                                       ctx->ntp_sync_interval_ms);
    }
  }

  receiver_log(LOG_INFO, ctx, "SEI Receiver source created");

  /* Start immediately in the background */
  start_receiver(ctx);

  return ctx;
}

/* Destroy the source */
static void receiver_source_destroy(void *data) {
  sei_receiver_source_t *ctx = (sei_receiver_source_t *)data;

  /* Stop the receiver */
  stop_receiver(ctx);
  multicam_status_unregister(ctx);
  if (ctx->playout_delay_filter) {
    obs_source_filter_remove(ctx->context, ctx->playout_delay_filter);
    obs_source_release(ctx->playout_delay_filter);
    ctx->playout_delay_filter = NULL;
  }

  /* avformat owns the SRT connection; no manual socket close */

  /* Destroy the decoder */
  if (ctx->codec_context) {
    AVCodecContext *codec_ctx = (AVCodecContext *)ctx->codec_context;
    avcodec_free_context(&codec_ctx);
  }

  /* Destroy the audio decoder */
  if (ctx->audio_codec_context) {
    AVCodecContext *actx = (AVCodecContext *)ctx->audio_codec_context;
    avcodec_free_context(&actx);
    ctx->audio_codec_context = NULL;
  }

  /* Destroy the NTP client */
  ntp_client_destroy(&ctx->ntp_client);

  /* Destroy the frame buffer */
  frame_buffer_destroy(&ctx->frame_buffer);
  if (ctx->audio_settings_lock_initialized) {
    pthread_mutex_destroy(&ctx->audio_settings_lock);
    ctx->audio_settings_lock_initialized = false;
  }

  /* Free the SwsContext */
  if (ctx->sws_ctx) {
    sws_freeContext(ctx->sws_ctx);
    ctx->sws_ctx = NULL;
  }

  receiver_log(LOG_INFO, ctx,
               "SEI Receiver destroyed (received: %llu, rendered: %llu, "
               "dropped: %llu, SEI found: %llu)",
               ctx->frames_received, ctx->frames_rendered, ctx->frames_dropped,
               ctx->sei_found_count);

  bfree(ctx);
}

/* Defaults */
static void receiver_source_defaults(obs_data_t *settings) {
  obs_data_set_default_string(settings, "srt_url", "srt://127.0.0.1:9000");
  obs_data_set_default_string(settings, "ntp_server", "time.windows.com");
  obs_data_set_default_int(settings, "ntp_port", 123);
  obs_data_set_default_bool(settings, "ntp_enabled", true);
  obs_data_set_default_string(settings, "hw_decoder", "none");
  obs_data_set_default_string(settings, "codec_format", "auto"); // auto-detect
  obs_data_set_default_int(settings, "ntp_drift_threshold", 50); // 50 ms
  obs_data_set_default_int(settings, "ntp_sync_interval",
                           10000); // default 10000 ms (10 s)
  obs_data_set_default_int(settings, "audio_offset_ms", 0);
}

static bool reset_sync_button(obs_properties_t *props, obs_property_t *property,
                              void *data);
static bool reset_picture_button(obs_properties_t *props,
                                 obs_property_t *property, void *data);

/* Properties */
static obs_properties_t *receiver_source_properties(void *data) {
  obs_properties_t *props = obs_properties_create();

  /* SRT URL */
  obs_properties_add_text(props, "srt_url", obs_module_text("SRTUrl"),
                          OBS_TEXT_DEFAULT);

  /* Hardware decoder choice */
  obs_property_t *hw_list =
      obs_properties_add_list(props, "hw_decoder", obs_module_text("HWDecoder"),
                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(hw_list, obs_module_text("HWDecoder.None"),
                               "none");
  obs_property_list_add_string(hw_list, obs_module_text("HWDecoder.QSV"),
                               "qsv");
  obs_property_list_add_string(hw_list, obs_module_text("HWDecoder.NVDEC"),
                               "nvdec");
  obs_property_list_add_string(hw_list, obs_module_text("HWDecoder.AMF"),
                               "amf");

  /* Codec format choice (manual beats auto-detect) */
  obs_property_t *codec_list = obs_properties_add_list(
      props, "codec_format", obs_module_text("CodecFormat"),
      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(codec_list, obs_module_text("CodecFormat.Auto"),
                               "auto");
  obs_property_list_add_string(codec_list, "H.264", "h264");
  obs_property_list_add_string(codec_list, "H.265 (HEVC)", "h265");
  obs_property_list_add_string(codec_list, "AV1", "av1");

  /* NTP settings group */
  obs_properties_add_group(props, "ntp_group", obs_module_text("NTPSettings"),
                           OBS_GROUP_NORMAL, NULL);

  /* NTP properties are added straight to props */
  obs_properties_add_bool(props, "ntp_enabled", obs_module_text("EnableNTP"));

  obs_properties_add_text(props, "ntp_server", obs_module_text("NTPServer"),
                          OBS_TEXT_DEFAULT);

  obs_properties_add_int(props, "ntp_port", obs_module_text("NTPPort"), 1,
                         65535, 1);

  obs_properties_add_int(props, "ntp_drift_threshold",
                         "NTP Drift Threshold (ms)", 10, 1000,
                         10); // 10 ms to 1000 ms

  obs_properties_add_int(props, "ntp_sync_interval", "NTP Sync Interval (ms)",
                         1000, 3600000, 1000); // 1 s to 1 hour

  obs_properties_add_int(props, "audio_offset_ms",
                         "Audio offset relative to video (ms)", -500, 500, 1);
  obs_properties_add_button2(props, "reset_sync", "Reset synchronization",
                             reset_sync_button, data);
  obs_properties_add_button2(props, "reset_picture", "Reset picture / reconnect",
                             reset_picture_button, data);

  /* Warning text */
  obs_properties_add_text(props, "ntp_interval_warning",
                          "NTP networking runs on a worker; failed requests "
                          "use retry backoff.",
                          OBS_TEXT_INFO);

  /* Status info (read-only) */
  obs_properties_add_text(props, "status", obs_module_text("Status"),
                          OBS_TEXT_INFO);

  /* Version info */
  obs_properties_add_text(props, "plugin_version", "v1.3.0",
                          OBS_TEXT_INFO);

  return props;
}

static bool reset_sync_button(obs_properties_t *props, obs_property_t *property,
                              void *data)
{
  UNUSED_PARAMETER(props);
  UNUSED_PARAMETER(property);
  sei_receiver_source_t *source = (sei_receiver_source_t *)data;
  if (source) {
    pthread_mutex_lock(&source->audio_settings_lock);
    source->reset_sync_requested = true;
    pthread_mutex_unlock(&source->audio_settings_lock);
  }
  return true;
}

static bool reset_picture_button(obs_properties_t *props,
                                 obs_property_t *property, void *data)
{
  UNUSED_PARAMETER(props);
  UNUSED_PARAMETER(property);
  sei_receiver_source_t *source = (sei_receiver_source_t *)data;
  if (source) {
    pthread_mutex_lock(&source->audio_settings_lock);
    source->reset_picture_requested = true;
    pthread_mutex_unlock(&source->audio_settings_lock);
  }
  return true;
}

/* Update settings */
static void receiver_source_update(void *data, obs_data_t *settings) {
  sei_receiver_source_t *ctx = (sei_receiver_source_t *)data;

  int64_t audio_offset_ns =
      (int64_t)obs_data_get_int(settings, "audio_offset_ms") * 1000000LL;
  pthread_mutex_lock(&ctx->audio_settings_lock);
  bool audio_offset_changed = ctx->audio_offset_ns != audio_offset_ns;
  ctx->audio_offset_ns = audio_offset_ns;
  pthread_mutex_unlock(&ctx->audio_settings_lock);
  if (audio_offset_changed) {
    multicam_status_set_audio_offset(ctx, (int32_t)(audio_offset_ns / 1000000LL));
  }

  bool settings_changed = false;

  /* Update the SRT URL */
  const char *srt_url = obs_data_get_string(settings, "srt_url");
  if (srt_url && srt_url[0] && strcmp(ctx->srt_url, srt_url) != 0) {
    /* A changed URL means restarting the receiver */
    receiver_log(LOG_INFO, ctx, "SRT URL changed, restarting...");
    stop_receiver(ctx);
    strncpy(ctx->srt_url, srt_url, sizeof(ctx->srt_url) - 1);
    settings_changed = true;
  }

  /* NTP settings may change while the receiver thread reads the clock. Stop
   * that reader before changing its snapshot. Updating the worker is local and
   * never joins it or performs DNS/socket I/O on OBS's UI thread. */
  bool ntp_enabled = obs_data_get_bool(settings, "ntp_enabled");
  const char *configured_ntp_server = obs_data_get_string(settings, "ntp_server");
  const char *ntp_server = configured_ntp_server && configured_ntp_server[0]
                               ? configured_ntp_server : "time.windows.com";
  uint16_t ntp_port = (uint16_t)obs_data_get_int(settings, "ntp_port");
  if (!ntp_port) ntp_port = 123;
  uint32_t ntp_interval = (uint32_t)obs_data_get_int(settings, "ntp_sync_interval");
  if (!ntp_interval) ntp_interval = 10000;
  if (ntp_enabled != ctx->ntp_enabled || strcmp(ntp_server, ctx->ntp_server) != 0 ||
      ntp_port != ctx->ntp_port || ntp_interval != ctx->ntp_sync_interval_ms) {
    stop_receiver(ctx);
    ctx->ntp_enabled = ntp_enabled;
    strncpy(ctx->ntp_server, ntp_server, sizeof(ctx->ntp_server) - 1);
    ctx->ntp_port = ntp_port;
    ctx->ntp_sync_interval_ms = ntp_interval;
    ctx->has_pts_offset = false;
    ctx->sync_method = SYNC_METHOD_UNKNOWN;
    ctx->absolute_sync_method = SYNC_METHOD_UNKNOWN;
    if (ctx->ntp_client.is_initialized) {
      ntp_client_update_settings(&ctx->ntp_client, ctx->ntp_server,
                                 ctx->ntp_port, ntp_interval, ntp_enabled);
      if (ntp_enabled && !ctx->ntp_client.sync_thread_active)
        ntp_client_start_background_sync(&ctx->ntp_client, ntp_interval);
    } else if (ntp_enabled &&
               ntp_client_init(&ctx->ntp_client, ctx->ntp_server,
                               ctx->ntp_port)) {
      ntp_client_start_background_sync(&ctx->ntp_client, ntp_interval);
    }
    receiver_log(LOG_INFO, ctx, "NTP worker settings updated (enabled: %s)",
                 ntp_enabled ? "yes" : "no");
    settings_changed = true;
  }

  /* Update the hardware decoder settings */
  const char *hw_decoder = obs_data_get_string(settings, "hw_decoder");
  if (hw_decoder && hw_decoder[0] &&
      strcmp(ctx->hw_decoder_type, hw_decoder) != 0) {
    /* A changed hardware decoder type means restarting the receiver */
    receiver_log(LOG_INFO, ctx,
                 "Hardware decoder changed from '%s' to '%s', restarting...",
                 ctx->hw_decoder_type, hw_decoder);

    /* Free the old hardware device context, if any */
    if (ctx->hw_device_ctx) {
      av_buffer_unref((AVBufferRef **)&ctx->hw_device_ctx);
      ctx->hw_device_ctx = NULL;
    }

    /* Stop the receiver */
    stop_receiver(ctx);

    /* Update the configuration */
    strncpy(ctx->hw_decoder_type, hw_decoder, sizeof(ctx->hw_decoder_type) - 1);
    ctx->hw_decode_enabled = (strcmp(hw_decoder, "none") != 0);

    settings_changed = true;
  }

  /* Update the codec format settings */
  const char *codec_format = obs_data_get_string(settings, "codec_format");
  if (codec_format && codec_format[0] &&
      strcmp(ctx->codec_format, codec_format) != 0) {
    /* A changed codec format means restarting the receiver */
    receiver_log(LOG_INFO, ctx,
                 "Codec format changed from '%s' to '%s', restarting...",
                 ctx->codec_format, codec_format);

    stop_receiver(ctx);
    strncpy(ctx->codec_format, codec_format, sizeof(ctx->codec_format) - 1);
    settings_changed = true;
  }

  /* If a settings change stopped it, restart now */
  if (settings_changed) {
    start_receiver(ctx);
  } else if (!ctx->is_connected) {
    /* Make sure it is always running */
    start_receiver(ctx);
  }

  receiver_log(LOG_INFO, ctx, "Settings updated");
}
/* Helper: tear down the connection's resources */
static void cleanup_connection(sei_receiver_source_t *source) {
  if (source->last_video_output_mono_ns)
    clear_output_video(source);
  if (source->format_context) {
    avformat_close_input((AVFormatContext **)&source->format_context);
    source->format_context = NULL;
  }
  if (source->codec_context) {
    avcodec_free_context((AVCodecContext **)&source->codec_context);
    source->codec_context = NULL;
  }
  if (source->sws_ctx) {
    sws_freeContext(source->sws_ctx);
    source->sws_ctx = NULL;
  }
  if (source->audio_codec_context) {
    avcodec_free_context((AVCodecContext **)&source->audio_codec_context);
    source->audio_codec_context = NULL;
  }
  if (source->audio_resampler) {
    SwrContext *resampler = (SwrContext *)source->audio_resampler;
    swr_free(&resampler);
    source->audio_resampler = NULL;
  }
  av_channel_layout_uninit(&source->audio_resampler_layout);
  source->is_connected = false;
  source->has_pts_offset = false;
  source->pts_offset = 0;
  source->sync_method = SYNC_METHOD_UNKNOWN;
  source->absolute_sync_method = SYNC_METHOD_UNKNOWN;
  source->first_ntp_time = 0;
  source->first_local_time = 0;
  source->last_video_pts_ns = AV_NOPTS_VALUE;
  source->last_video_output_mono_ns = 0;
  source->video_output_cleared = true;
  source->smoothed_input_latency_ns = 0;
  multicam_status_set_sync_latency(source, 0);
  schedule_playout_delay(source, 0, true);
  multicam_status_set_connection(source, false, NULL, false, 0, 0);
  receiver_log(LOG_INFO, source, "Connection closed and resources freed");
}

/* Helper: try to connect */
static bool try_connect(sei_receiver_source_t *source) {
  receiver_log(LOG_INFO, source, "Attempting to connect to: %s",
               source->srt_url);

  AVFormatContext *fmt_ctx = avformat_alloc_context();
  if (!fmt_ctx)
    return false;
  fmt_ctx->interrupt_callback.callback = interrupt_if_video_stale;
  fmt_ctx->interrupt_callback.opaque = source;

  /* Set a timeout so it cannot block for long */
  AVDictionary *options = NULL;
  av_dict_set(&options, "timeout", "2000000", 0); /* 2 s timeout */

  /* Use the configured SRT URL as-is (it may carry ?streamid=xxx etc.) */
  if (avformat_open_input(&fmt_ctx, source->srt_url, NULL, &options) < 0) {
    receiver_log(LOG_WARNING, source,
                 "Failed to open input (sender might be offline)");
    avformat_free_context(fmt_ctx);
    if (options)
      av_dict_free(&options);
    return false;
  }
  if (options)
    av_dict_free(&options);

  if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
    receiver_log(LOG_ERROR, source, "Failed to find stream info");
    avformat_close_input(&fmt_ctx);
    return false;
  }

  /* Find the video stream */
  int video_idx = -1;
  for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_idx = i;
      break;
    }
  }

  if (video_idx == -1) {
    receiver_log(LOG_ERROR, source, "No video stream found");
    avformat_close_input(&fmt_ctx);
    return false;
  }

  source->video_stream_index = video_idx;
  AVStream *vstream = fmt_ctx->streams[video_idx];

  /* Initialise the hardware device, if enabled */
  if (source->hw_decode_enabled) {
    init_hw_device(source);
  }

  /* Decide the codec: a manual choice beats auto-detect */
  enum AVCodecID codec_id;
  const char *codec_name;

  if (strcmp(source->codec_format, "auto") != 0) {
    /* The user picked a codec format */
    if (strcmp(source->codec_format, "h264") == 0) {
      codec_id = AV_CODEC_ID_H264;
    } else if (strcmp(source->codec_format, "h265") == 0 ||
               strcmp(source->codec_format, "hevc") == 0) {
      codec_id = AV_CODEC_ID_HEVC;
    } else if (strcmp(source->codec_format, "av1") == 0) {
      codec_id = AV_CODEC_ID_AV1;
    } else {
      /* Unrecognised format; fall back to auto-detect */
      codec_id = vstream->codecpar->codec_id;
    }
    codec_name = avcodec_get_name(codec_id);
    receiver_log(LOG_INFO, source, "Using manually selected codec: %s (ID: %d)",
                 codec_name, codec_id);
  } else {
    /* Auto-detect the stream's codec */
    codec_id = vstream->codecpar->codec_id;
    codec_name = avcodec_get_name(codec_id);
    receiver_log(LOG_INFO, source, "Auto-detected codec: %s (ID: %d)",
                 codec_name, codec_id);
  }

  /* Initialise the video decoder */
  const AVCodec *codec = avcodec_find_decoder(codec_id);
  if (!codec) {
    receiver_log(LOG_ERROR, source, "Video decoder not found for codec: %s",
                 codec_name);
    avformat_close_input(&fmt_ctx);
    return false;
  }

  AVCodecContext *cctx = avcodec_alloc_context3(codec);

  /* Copy stream parameters (resolution, profile) even for a manual codec */
  /* but use the stream's codec_id only when auto-detecting */
  if (strcmp(source->codec_format, "auto") == 0) {
    /* Auto-detect: take the stream's parameters wholesale */
    avcodec_parameters_to_context(cctx, vstream->codecpar);
  } else {
    /* Manual: keep the stream's parameters but replace the codec type */
    /* copy the stream's parameters first */
    avcodec_parameters_to_context(cctx, vstream->codecpar);

    /* then override codec_id with the user's choice */
    cctx->codec_id = codec_id;

    receiver_log(LOG_INFO, source,
                 "Applied stream parameters with manual codec override");
  }

  /* Make decoder PTS and best-effort timestamps use the stream's time base. */
  cctx->pkt_timebase = vstream->time_base;

  /* Configure hardware decode */
  if (source->hw_decode_enabled && source->hw_device_ctx) {
    cctx->hw_device_ctx = av_buffer_ref((AVBufferRef *)source->hw_device_ctx);
    cctx->get_format = get_hw_format;
    receiver_log(LOG_INFO, source, "Hardware decoder configured");
  }

  /* Check the extradata (VPS/SPS/PPS for H.265) */
  if (cctx->extradata && cctx->extradata_size > 0) {
    receiver_log(LOG_INFO, source, "Decoder has extradata: %d bytes",
                 cctx->extradata_size);
  } else {
    receiver_log(LOG_WARNING, source,
                 "Decoder has NO extradata - H.265 may fail to decode!");
  }

  if (avcodec_open2(cctx, codec, NULL) < 0) {
    receiver_log(LOG_ERROR, source, "Failed to open video codec");
    avcodec_free_context(&cctx);
    avformat_close_input(&fmt_ctx);
    return false;
  }

  source->format_context = fmt_ctx;
  source->codec_context = cctx;
  source->width = cctx->width;
  source->height = cctx->height;

  /* Find the audio stream (optional) */
  source->audio_stream_index = -1;
  int audio_idx = -1;
  for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      audio_idx = i;
      break;
    }
  }

  if (audio_idx != -1) {
    AVStream *astream = fmt_ctx->streams[audio_idx];
    const AVCodec *acodec = avcodec_find_decoder(astream->codecpar->codec_id);
    if (acodec) {
      AVCodecContext *actx = avcodec_alloc_context3(acodec);
      avcodec_parameters_to_context(actx, astream->codecpar);
      actx->pkt_timebase = astream->time_base;
      if (avcodec_open2(actx, acodec, NULL) >= 0) {
        source->audio_codec_context = actx;
        source->audio_stream_index = audio_idx;
        source->audio_channels = actx->ch_layout.nb_channels;
        source->audio_sample_rate = actx->sample_rate;
        /* decode_audio always converts into this OBS sample layout. */
        source->audio_format = AUDIO_FORMAT_FLOAT_PLANAR;

        receiver_log(LOG_INFO, source, "Audio stream opened (idx: %d)",
                     audio_idx);
      } else {
        avcodec_free_context(&actx);
      }
    }
  }

  source->is_connected = true;
  multicam_status_set_connection(source, true, codec_name,
                                 source->audio_stream_index >= 0,
                                 source->width, source->height);
  receiver_log(LOG_INFO, source, "Connected successfully!");
  return true;
}

/* SRT receive thread (owns connection management and reads) */
static void *srt_receive_thread(void *data) {
  sei_receiver_source_t *source = (sei_receiver_source_t *)data;
  receiver_log(LOG_INFO, source, "Thread started (Auto-Reconnect Mode)");

  // struct os_timespec ts; // Removed unused variable
  // ts.tv_sec = 1;
  // ts.tv_nsec = 0;

  AVPacket *packet = av_packet_alloc();

  while (source->thread_active) {
    pthread_mutex_lock(&source->audio_settings_lock);
    bool reset_picture = source->reset_picture_requested;
    source->reset_picture_requested = false;
    bool reset_sync = source->reset_sync_requested;
    source->reset_sync_requested = false;
    pthread_mutex_unlock(&source->audio_settings_lock);

    if (reset_picture) {
      receiver_log(LOG_WARNING, source,
                   "Picture reset requested; reconnecting decoder and input");
      cleanup_connection(source);
    }
    if (reset_sync) {
      source->has_pts_offset = false;
      source->pts_offset = 0;
      source->sync_method = SYNC_METHOD_UNKNOWN;
      source->absolute_sync_method = SYNC_METHOD_UNKNOWN;
      source->last_video_pts_ns = AV_NOPTS_VALUE;
      source->smoothed_input_latency_ns = 0;
      multicam_status_set_sync_latency(source, 0);
      schedule_playout_delay(source, 0, true);
      receiver_log(LOG_INFO, source,
                   "Synchronization reset requested; waiting for a fresh absolute timestamp");
    }

    /* 1. Not connected: try to connect */
    if (!source->is_connected) {
      if (try_connect(source)) {
        /* connected, so enter the read loop */
      } else {
        /* connect failed; wait and retry */
        /* pthread_testcancel(); // Allow exit (Not supported in w32-pthreads) */
        os_sleep_ms(2000);    // Sleep 2s
        continue;
      }
    }

    /* 2. Read data (connected) */
    if (source->is_connected && source->format_context) {
      int ret =
          av_read_frame((AVFormatContext *)source->format_context, packet);
      clear_if_video_stale(source);

      if (ret < 0) {
        /* Read error or EOF -> drop the connection and reconnect */
        if (ret == AVERROR_EOF) {
          receiver_log(LOG_INFO, source, "End of Stream");
        } else {
          receiver_log(LOG_WARNING, source, "Read Error: %d", ret);
        }
        cleanup_connection(source);
        continue; /* back to the top of the loop, triggering a reconnect */
      }

      multicam_status_add_packet(source, (size_t)packet->size,
                                 os_gettime_ns());

      /* 3. Handle the packet */
      static uint64_t video_packet_count = 0;
      static uint64_t audio_packet_count = 0;

      if (packet->stream_index == source->video_stream_index) {
        video_packet_count++;
        if (video_packet_count % 30 == 1) {
          receiver_log(LOG_INFO, source,
                       "Video packet received: #%llu, size=%d",
                       video_packet_count, packet->size);
        }
        video_frame_data_t frame = {0};
        if (decode_and_extract_sei(source, packet, &frame)) {
          source->frames_rendered++;
        }
      } else if (source->audio_stream_index >= 0 &&
                 packet->stream_index == source->audio_stream_index) {
        audio_packet_count++;
        if (audio_packet_count % 300 == 1) {
          receiver_log(LOG_INFO, source, "Audio packet received: #%llu",
                       audio_packet_count);
        }
        decode_audio(source, packet);
      }

      av_packet_unref(packet);
    }
  }

  av_packet_free(&packet);
  cleanup_connection(source);
  receiver_log(LOG_INFO, source, "Thread stopped");
  return NULL;
}

/* Start the receiver (thread only) */
static void start_receiver(void *data) {
  sei_receiver_source_t *ctx = (sei_receiver_source_t *)data;

  if (ctx->thread_active)
    return;

  receiver_log(LOG_INFO, ctx, "Starting background thread...");
  ctx->thread_active = true;
  pthread_create(&ctx->receive_thread, NULL, srt_receive_thread, ctx);
}

/* Stop the receiver (stop the thread) */
static void stop_receiver(void *data) {
  sei_receiver_source_t *ctx = (sei_receiver_source_t *)data;

  if (!ctx->thread_active)
    return;

  receiver_log(LOG_INFO, ctx, "Stopping background thread...");
  ctx->thread_active = false;
  pthread_join(ctx->receive_thread, NULL);

  /* Resources are cleaned up at end of thread,
     but we can ensure safety here if needed */
}

/* Note: ASYNC_VIDEO needs no video_render callback */
/* frames go straight to OBS via obs_source_output_video() */

/* Video width */
static uint32_t receiver_source_get_width(void *data) {
  sei_receiver_source_t *ctx = (sei_receiver_source_t *)data;
  return ctx->width ? ctx->width : 1920; /* 1920 by default */
}

/* Video height */
static uint32_t receiver_source_get_height(void *data) {
  sei_receiver_source_t *ctx = (sei_receiver_source_t *)data;
  return ctx->height ? ctx->height : 1080; /* 1080 by default */
}

/*============================================================================
 * Source plugin info struct
 *============================================================================*/

struct obs_source_info sei_receiver_source_info = {
    .id = "sei_receiver_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    /* ASYNC_VIDEO: frames pushed via obs_source_output_video(); no video_render
     * OBS_SOURCE_DO_NOT_DUPLICATE removed so OBS copies data, avoiding UAF
     */
    .output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
    .get_name = receiver_source_getname,
    .create = receiver_source_create,
    .destroy = receiver_source_destroy,
    .get_defaults = receiver_source_defaults,
    .get_properties = receiver_source_properties,
    .update = receiver_source_update,
    /* .activate = receiver_source_activate, */
    /* .deactivate = receiver_source_deactivate, */
    /* Note: ASYNC_VIDEO does not set video_render */
    .get_width = receiver_source_get_width,
    .get_height = receiver_source_get_height,
};


