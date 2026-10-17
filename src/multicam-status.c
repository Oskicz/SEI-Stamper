#include "multicam-status.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

typedef struct status_slot {
  void *key;
  multicam_source_status_t value;
  uint64_t packet_window_bytes;
  uint64_t packet_window_start_ns;
  uint64_t bitrate_samples[5];
  uint8_t bitrate_sample_count;
  uint8_t bitrate_sample_next;
  uint64_t fps_window_start_ns;
  uint64_t fps_window_frames;
} status_slot_t;

static pthread_mutex_t status_lock = PTHREAD_MUTEX_INITIALIZER;
static status_slot_t slots[MULTICAM_STATUS_MAX_SOURCES];

static status_slot_t *find_slot(void *key)
{
  for (size_t i = 0; i < MULTICAM_STATUS_MAX_SOURCES; ++i)
    if (slots[i].key == key) return &slots[i];
  return NULL;
}

void multicam_status_register(void *key, const char *name)
{
  if (!key) return;
  pthread_mutex_lock(&status_lock);
  if (!find_slot(key)) {
    for (size_t i = 0; i < MULTICAM_STATUS_MAX_SOURCES; ++i) {
      if (!slots[i].key) {
        memset(&slots[i], 0, sizeof(slots[i]));
        slots[i].key = key;
        if (name) snprintf(slots[i].value.name, sizeof(slots[i].value.name), "%s", name);
        slots[i].value.sync_method = MULTICAM_SYNC_UNKNOWN;
        break;
      }
    }
  }
  pthread_mutex_unlock(&status_lock);
}

void multicam_status_unregister(void *key)
{
  if (!key) return;
  pthread_mutex_lock(&status_lock);
  status_slot_t *slot = find_slot(key);
  if (slot) memset(slot, 0, sizeof(*slot));
  pthread_mutex_unlock(&status_lock);
}

void multicam_status_set_connection(void *key, bool connected,
                                    const char *codec, bool has_audio,
                                    uint32_t width, uint32_t height)
{
  pthread_mutex_lock(&status_lock);
  status_slot_t *slot = find_slot(key);
  if (slot) {
    slot->value.connected = connected;
    slot->value.has_audio = connected && has_audio;
    slot->value.width = width;
    slot->value.height = height;
    if (codec) snprintf(slot->value.codec, sizeof(slot->value.codec), "%s", codec);
    if (!connected) {
      slot->value.ntp_fresh = false;
      slot->value.sync_method = MULTICAM_SYNC_UNKNOWN;
      slot->value.last_video_mono_ns = 0;
      slot->value.last_audio_mono_ns = 0;
      slot->value.last_packet_mono_ns = 0;
      slot->value.last_absolute_unix_ns = 0;
      slot->value.observed_latency_ns = 0;
      slot->value.rx_bitrate_bps = 0;
      slot->value.rx_bitrate_5s_bps = 0;
      slot->packet_window_bytes = 0;
      slot->packet_window_start_ns = 0;
      memset(slot->bitrate_samples, 0, sizeof(slot->bitrate_samples));
      slot->bitrate_sample_count = 0;
      slot->bitrate_sample_next = 0;
    }
  }
  pthread_mutex_unlock(&status_lock);
}

void multicam_status_add_packet(void *key, size_t bytes, uint64_t now_ns)
{
  pthread_mutex_lock(&status_lock);
  status_slot_t *slot = find_slot(key);
  if (slot) {
    slot->value.rx_bytes_total += bytes;
    slot->value.last_packet_mono_ns = now_ns;
    slot->packet_window_bytes += bytes;
    if (!slot->packet_window_start_ns) slot->packet_window_start_ns = now_ns;
    uint64_t elapsed = now_ns >= slot->packet_window_start_ns
                           ? now_ns - slot->packet_window_start_ns : 0;
    if (elapsed >= 1000000000ULL) {
      slot->value.rx_bitrate_bps =
          (uint64_t)((long double)slot->packet_window_bytes * 8.0L * 1000000000.0L / elapsed);
      slot->bitrate_samples[slot->bitrate_sample_next] = slot->value.rx_bitrate_bps;
      slot->bitrate_sample_next = (uint8_t)((slot->bitrate_sample_next + 1) % 5);
      if (slot->bitrate_sample_count < 5) slot->bitrate_sample_count++;
      uint64_t sum = 0;
      for (uint8_t i = 0; i < slot->bitrate_sample_count; ++i)
        sum += slot->bitrate_samples[i];
      slot->value.rx_bitrate_5s_bps = sum / slot->bitrate_sample_count;
      slot->packet_window_bytes = 0;
      slot->packet_window_start_ns = now_ns;
    }
  }
  pthread_mutex_unlock(&status_lock);
}

void multicam_status_video(void *key, multicam_status_sync_method_t method,
                           bool ntp_fresh, int64_t absolute_unix_ns,
                           uint64_t observed_latency_ns,
                           uint32_t width, uint32_t height,
                           int64_t video_offset_ns, uint64_t now_ns)
{
  pthread_mutex_lock(&status_lock);
  status_slot_t *slot = find_slot(key);
  if (slot) {
    slot->value.video_frames++;
    slot->value.last_video_mono_ns = now_ns;
    slot->value.sync_method = method;
    slot->value.ntp_fresh = ntp_fresh;
    slot->value.last_absolute_unix_ns = absolute_unix_ns > 0 ? (uint64_t)absolute_unix_ns : 0;
    slot->value.observed_latency_ns = ntp_fresh ? observed_latency_ns : 0;
    slot->value.width = width;
    slot->value.height = height;
    slot->value.video_offset_ns = video_offset_ns;
    if (!slot->fps_window_start_ns) {
      slot->fps_window_start_ns = now_ns;
      slot->fps_window_frames = slot->value.video_frames;
    } else if (now_ns > slot->fps_window_start_ns &&
               now_ns - slot->fps_window_start_ns >= 1000000000ULL) {
      uint64_t count = slot->value.video_frames - slot->fps_window_frames;
      slot->value.fps_milli = (uint32_t)((long double)count * 1000000000.0L * 1000.0L /
                                         (now_ns - slot->fps_window_start_ns));
      slot->fps_window_start_ns = now_ns;
      slot->fps_window_frames = slot->value.video_frames;
    }
  }
  pthread_mutex_unlock(&status_lock);
}

void multicam_status_audio(void *key, uint64_t now_ns)
{
  pthread_mutex_lock(&status_lock);
  status_slot_t *slot = find_slot(key);
  if (slot) {
    slot->value.audio_frames++;
    slot->value.last_audio_mono_ns = now_ns;
  }
  pthread_mutex_unlock(&status_lock);
}

void multicam_status_set_audio_offset(void *key, int32_t offset_ms)
{
  pthread_mutex_lock(&status_lock);
  status_slot_t *slot = find_slot(key);
  if (slot) slot->value.audio_offset_ms = offset_ms;
  pthread_mutex_unlock(&status_lock);
}

void multicam_status_set_decode_errors(void *key, uint64_t errors)
{
  pthread_mutex_lock(&status_lock);
  status_slot_t *slot = find_slot(key);
  if (slot) slot->value.decode_errors = errors;
  pthread_mutex_unlock(&status_lock);
}

void multicam_status_set_audio_decode_errors(void *key, uint64_t errors)
{
  pthread_mutex_lock(&status_lock);
  status_slot_t *slot = find_slot(key);
  if (slot) slot->value.audio_decode_errors = errors;
  pthread_mutex_unlock(&status_lock);
}

size_t multicam_status_snapshot(multicam_source_status_t *out, size_t capacity)
{
  if (!out || !capacity) return 0;
  pthread_mutex_lock(&status_lock);
  size_t count = 0;
  for (size_t i = 0; i < MULTICAM_STATUS_MAX_SOURCES && count < capacity; ++i)
    if (slots[i].key) out[count++] = slots[i].value;
  pthread_mutex_unlock(&status_lock);
  return count;
}

static bool is_recent(uint64_t stamp, uint64_t now_ns)
{
  return stamp && now_ns >= stamp && now_ns - stamp <= 2000000000ULL;
}

bool multicam_status_source_ready(const multicam_source_status_t *s,
                                  uint64_t now_ns)
{
  if (!s || !s->connected || !is_recent(s->last_packet_mono_ns, now_ns) ||
      !is_recent(s->last_video_mono_ns, now_ns) ||
      !s->rx_bitrate_bps || !s->ntp_fresh || !s->last_absolute_unix_ns ||
      (s->sync_method != MULTICAM_SYNC_SEI_STAMPER_NTP &&
       s->sync_method != MULTICAM_SYNC_MOBLIN_TIMECODE))
    return false;
  return !s->has_audio || is_recent(s->last_audio_mono_ns, now_ns);
}

bool multicam_status_group_aligned(const multicam_source_status_t *statuses,
                                   size_t count, uint64_t now_ns,
                                   uint64_t tolerance_ns,
                                   uint64_t *spread_ns)
{
  if (!statuses || count == 0) return false;
  uint64_t minimum = UINT64_MAX, maximum = 0;
  bool all_ready = true;
  for (size_t i = 0; i < count; ++i) {
    const multicam_source_status_t *s = &statuses[i];
    if (!multicam_status_source_ready(s, now_ns)) all_ready = false;
    uint64_t t = s->last_absolute_unix_ns;
    if (t < s->playout_delay_ns) return false;
    t -= s->playout_delay_ns;
    if (!t) return false;
    if (t < minimum) minimum = t;
    if (t > maximum) maximum = t;
  }
  uint64_t spread = maximum - minimum;
  if (spread_ns) *spread_ns = spread;
  return all_ready && spread <= tolerance_ns;
}

multicam_sync_quality_t multicam_status_sync_quality(uint64_t spread_ns)
{
  if (spread_ns < 40000000ULL) return MULTICAM_SYNC_QUALITY_EXCELLENT;
  if (spread_ns < 100000000ULL) return MULTICAM_SYNC_QUALITY_GOOD;
  if (spread_ns < 250000000ULL) return MULTICAM_SYNC_QUALITY_WARNING;
  return MULTICAM_SYNC_QUALITY_BAD;
}

uint64_t multicam_status_playout_delay(uint64_t group_max_latency_ns,
                                       uint64_t source_latency_ns)
{
  return group_max_latency_ns > source_latency_ns
             ? group_max_latency_ns - source_latency_ns : 0;
}

uint64_t multicam_status_group_max_latency_ns(uint64_t now_ns)
{
  uint64_t maximum = 0;
  pthread_mutex_lock(&status_lock);
  for (size_t i = 0; i < MULTICAM_STATUS_MAX_SOURCES; ++i) {
    const multicam_source_status_t *s = &slots[i].value;
    if (slots[i].key && s->connected && s->ntp_fresh &&
        (s->sync_method == MULTICAM_SYNC_SEI_STAMPER_NTP ||
         s->sync_method == MULTICAM_SYNC_MOBLIN_TIMECODE) &&
        is_recent(s->last_video_mono_ns, now_ns) &&
        s->observed_latency_ns > maximum)
      maximum = s->observed_latency_ns;
  }
  pthread_mutex_unlock(&status_lock);
  return maximum;
}

void multicam_status_set_playout_delay(void *key, uint64_t delay_ns)
{
  pthread_mutex_lock(&status_lock);
  status_slot_t *slot = find_slot(key);
  if (slot) slot->value.playout_delay_ns = delay_ns;
  pthread_mutex_unlock(&status_lock);
}


