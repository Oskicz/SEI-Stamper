#pragma once

#include <stdint.h>
#include <stdbool.h>

#define RECEIVER_PLAYOUT_DELAY_TOLERANCE_NS 30000000ULL
#define RECEIVER_PLAYOUT_DELAY_MIN_CHANGE_NS 100000000ULL
#define RECEIVER_PLAYOUT_DELAY_SETTLE_NS 750000000ULL
#define RECEIVER_PLAYOUT_DELAY_COOLDOWN_NS 3000000000ULL

typedef struct receiver_playout_delay_control {
  bool initialized;
  bool has_pending;
  uint64_t applied_ns;
  uint64_t pending_ns;
  uint64_t pending_since_ns;
  uint64_t last_update_ns;
} receiver_playout_delay_control_t;

static inline uint64_t receiver_add_playout_latency_ns(uint64_t timestamp_ns,
                                                        uint64_t latency_ns)
{
  return latency_ns > UINT64_MAX - timestamp_ns ? UINT64_MAX
                                                 : timestamp_ns + latency_ns;
}

static inline void receiver_playout_delay_reset(
    receiver_playout_delay_control_t *control)
{
  if (control) *control = (receiver_playout_delay_control_t){0};
}

/* OBS's async-delay filter clears its frame queue on every settings update.
 * Only publish a new target after it has stayed stable, and rate-limit
 * corrections so small latency jitter cannot repeatedly empty the queue. */
static inline bool receiver_playout_delay_consider(
    receiver_playout_delay_control_t *control, uint64_t target_ns,
    uint64_t now_ns, uint64_t *applied_ns)
{
  if (!control || !applied_ns) return false;
  if (!control->initialized) {
    control->initialized = true;
    control->applied_ns = target_ns;
    control->last_update_ns = now_ns;
    control->has_pending = false;
    *applied_ns = target_ns;
    return true;
  }

  uint64_t applied_delta = target_ns > control->applied_ns
                               ? target_ns - control->applied_ns
                               : control->applied_ns - target_ns;
  if (applied_delta < RECEIVER_PLAYOUT_DELAY_MIN_CHANGE_NS) {
    control->has_pending = false;
    *applied_ns = control->applied_ns;
    return false;
  }

  if (!control->has_pending) {
    control->has_pending = true;
    control->pending_ns = target_ns;
    control->pending_since_ns = now_ns;
    *applied_ns = control->applied_ns;
    return false;
  }

  uint64_t pending_delta = target_ns > control->pending_ns
                               ? target_ns - control->pending_ns
                               : control->pending_ns - target_ns;
  if (pending_delta > RECEIVER_PLAYOUT_DELAY_TOLERANCE_NS ||
      now_ns < control->pending_since_ns) {
    control->pending_ns = target_ns;
    control->pending_since_ns = now_ns;
    *applied_ns = control->applied_ns;
    return false;
  }

  if (now_ns - control->pending_since_ns < RECEIVER_PLAYOUT_DELAY_SETTLE_NS ||
      now_ns < control->last_update_ns ||
      now_ns - control->last_update_ns < RECEIVER_PLAYOUT_DELAY_COOLDOWN_NS) {
    *applied_ns = control->applied_ns;
    return false;
  }

  control->applied_ns = target_ns;
  control->last_update_ns = now_ns;
  control->has_pending = false;
  *applied_ns = target_ns;
  return true;
}

static inline bool receiver_keep_absolute_sync(bool has_absolute_anchor,
                                               bool ntp_enabled,
                                               bool ntp_fresh)
{
  return has_absolute_anchor && ntp_enabled && ntp_fresh;
}

static inline bool receiver_monotonic_to_unix_ns(int64_t frame_mono_ns,
                                                  int64_t anchor_mono_ns,
                                                  int64_t anchor_unix_ns,
                                                  int64_t *unix_ns)
{
  if (!unix_ns ||
      (anchor_mono_ns < 0 && frame_mono_ns > INT64_MAX + anchor_mono_ns) ||
      (anchor_mono_ns > 0 && frame_mono_ns < INT64_MIN + anchor_mono_ns))
    return false;
  int64_t delta = frame_mono_ns - anchor_mono_ns;
  if ((delta > 0 && anchor_unix_ns > INT64_MAX - delta) ||
      (delta < 0 && anchor_unix_ns < INT64_MIN - delta))
    return false;
  *unix_ns = anchor_unix_ns + delta;
  return true;
}

/* Convert a stream PTS to the receiver's monotonic nanosecond time scale. */
static inline int64_t receiver_pts_to_ns(int64_t pts, int time_base_num,
                                         int time_base_den)
{
  if (time_base_num <= 0 || time_base_den <= 0)
    return 0;

  /* long double avoids overflowing the intermediate pts * num * 1e9. */
  long double ns = (long double)pts * (long double)time_base_num * 1000000000.0L /
                   (long double)time_base_den;
  if (ns > (long double)INT64_MAX)
    return INT64_MAX;
  if (ns < (long double)INT64_MIN)
    return INT64_MIN;
  return (int64_t)(ns >= 0 ? ns + 0.5L : ns - 0.5L);
}

static inline bool receiver_pts_has_backward_reset(int64_t previous_pts_ns,
                                                    int64_t current_pts_ns,
                                                    uint64_t threshold_ns)
{
  return current_pts_ns < previous_pts_ns &&
         (long double)previous_pts_ns - (long double)current_pts_ns >
             (long double)threshold_ns;
}

static inline bool receiver_video_is_stale(uint64_t last_frame_mono_ns,
                                           uint64_t now_mono_ns,
                                           uint64_t timeout_ns)
{
  return last_frame_mono_ns && now_mono_ns >= last_frame_mono_ns &&
         now_mono_ns - last_frame_mono_ns >= timeout_ns;
}

static inline bool receiver_ntp_to_unix_ns(uint32_t seconds, uint32_t fraction,
                                           int64_t *unix_ns)
{
  const int64_t ntp_epoch_delta = 2208988800LL;
  if (!unix_ns || (int64_t)seconds < ntp_epoch_delta)
    return false;
  int64_t unix_seconds = (int64_t)seconds - ntp_epoch_delta;
  *unix_ns = unix_seconds * 1000000000LL +
             (int64_t)(((uint64_t)fraction * 1000000000ULL) >> 32);
  return true;
}

static inline bool receiver_ntp_to_monotonic_ns(uint32_t frame_seconds,
                                                uint32_t frame_fraction,
                                                uint32_t anchor_seconds,
                                                uint32_t anchor_fraction,
                                                int64_t monotonic_anchor_ns,
                                                int64_t *display_ns)
{
  int64_t frame_ns, anchor_ns;
  if (!display_ns ||
      !receiver_ntp_to_unix_ns(frame_seconds, frame_fraction, &frame_ns) ||
      !receiver_ntp_to_unix_ns(anchor_seconds, anchor_fraction, &anchor_ns))
    return false;
  int64_t delta = frame_ns - anchor_ns;
  if ((delta > 0 && monotonic_anchor_ns > INT64_MAX - delta) ||
      (delta < 0 && monotonic_anchor_ns < INT64_MIN - delta))
    return false;
  *display_ns = monotonic_anchor_ns + delta;
  return true;
}

/* Apply a user calibration to an already mapped OBS monotonic timestamp.
 * Positive offset delays audio; negative offset advances it. */
static inline uint64_t receiver_audio_timestamp_with_offset(int64_t timestamp_ns,
                                                            int64_t offset_ns)
{
  if (timestamp_ns < 0) return 0;
  if (offset_ns > 0 && timestamp_ns > INT64_MAX - offset_ns)
    return (uint64_t)INT64_MAX;
  if (offset_ns < 0 &&
      (offset_ns == INT64_MIN || timestamp_ns < -offset_ns))
    return 0;
  int64_t adjusted = timestamp_ns + offset_ns;
  return adjusted > 0 ? (uint64_t)adjusted : 0;
}

static inline uint32_t receiver_ntp_max_age_seconds(uint32_t sync_interval_ms)
{
  uint64_t interval_seconds = ((uint64_t)sync_interval_ms + 999ULL) / 1000ULL;
  uint64_t max_age = interval_seconds * 3ULL;
  if (max_age < 120ULL) max_age = 120ULL;
  if (max_age > 3600ULL) max_age = 3600ULL;
  return (uint32_t)max_age;
}

