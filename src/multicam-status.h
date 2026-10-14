#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MULTICAM_STATUS_MAX_SOURCES 64

typedef enum multicam_status_sync_method {
  MULTICAM_SYNC_UNKNOWN,
  MULTICAM_SYNC_PTS_FALLBACK,
  MULTICAM_SYNC_SEI_STAMPER_NTP,
  MULTICAM_SYNC_MOBLIN_TIMECODE
} multicam_status_sync_method_t;

typedef struct multicam_source_status {
  char name[128];
  char codec[16];
  bool connected;
  bool has_audio;
  bool ntp_fresh;
  int32_t audio_offset_ms;
  multicam_status_sync_method_t sync_method;
  uint32_t width, height;
  uint64_t video_frames, audio_frames;
  uint64_t decode_errors;
  uint64_t last_packet_mono_ns;
  uint64_t last_video_mono_ns, last_audio_mono_ns;
  uint64_t last_absolute_unix_ns;
  uint64_t observed_latency_ns;
  uint32_t srt_rcvlatency_ms;
  uint64_t rx_bytes_total, rx_bitrate_bps;
  uint64_t rx_bitrate_5s_bps;
  int64_t video_offset_ns;
  uint32_t fps_milli;
} multicam_source_status_t;

void multicam_status_register(void *key, const char *name);
void multicam_status_unregister(void *key);
void multicam_status_set_connection(void *key, bool connected,
                                    const char *codec, bool has_audio,
                                    uint32_t width, uint32_t height);
void multicam_status_add_packet(void *key, size_t bytes, uint64_t now_ns);
void multicam_status_video(void *key, multicam_status_sync_method_t method,
                           bool ntp_fresh, int64_t absolute_unix_ns,
                           uint64_t observed_latency_ns,
                           uint32_t width, uint32_t height,
                           int64_t video_offset_ns, uint64_t now_ns);
void multicam_status_audio(void *key, uint64_t now_ns);
void multicam_status_set_audio_offset(void *key, int32_t offset_ms);
void multicam_status_set_srt_latency(void *key, uint32_t latency_ms);
uint64_t multicam_status_max_observed_latency_ns(uint64_t now_ns);
void multicam_status_set_decode_errors(void *key, uint64_t errors);
size_t multicam_status_snapshot(multicam_source_status_t *out, size_t capacity);

/* A preflight passes only when every required signal is present. This is
 * intentionally conservative: PTS fallback cannot pass absolute-sync checks. */
bool multicam_status_source_ready(const multicam_source_status_t *status,
                                  uint64_t now_ns);
bool multicam_status_group_aligned(const multicam_source_status_t *statuses,
                                   size_t count, uint64_t now_ns,
                                   uint64_t tolerance_ns,
                                   uint64_t *spread_ns);

#ifdef __cplusplus
}
#endif

