#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct receiver_playout_delay_control {
  uint64_t candidate_delay_ns;
  uint64_t candidate_since_ns;
  uint64_t applied_delay_ns;
  uint64_t last_applied_ns;
  bool candidate_valid;
  bool has_applied;
} receiver_playout_delay_control_t;

/* Defer OBS async-delay updates until the requested delay has settled. OBS's
 * built-in filter flushes its frame queue on every update, so applying jitter
 * samples directly can repeatedly blank the source. */
static inline void receiver_playout_delay_control_reset(
    receiver_playout_delay_control_t *control)
{
  if (!control) return;
  control->candidate_delay_ns = 0;
  control->candidate_since_ns = 0;
  control->applied_delay_ns = 0;
  control->last_applied_ns = 0;
  control->candidate_valid = false;
  control->has_applied = false;
}

static inline bool receiver_playout_delay_control_consider(
    receiver_playout_delay_control_t *control, uint64_t requested_delay_ns,
    uint64_t now_ns, uint64_t stability_tolerance_ns,
    uint64_t settle_window_ns, uint64_t significant_change_ns,
    uint64_t update_cooldown_ns)
{
  if (!control) return false;
  uint64_t candidate_delta = requested_delay_ns > control->candidate_delay_ns
      ? requested_delay_ns - control->candidate_delay_ns
      : control->candidate_delay_ns - requested_delay_ns;
  if (!control->candidate_valid ||
      candidate_delta > stability_tolerance_ns ||
      now_ns < control->candidate_since_ns) {
    control->candidate_delay_ns = requested_delay_ns;
    control->candidate_since_ns = now_ns;
    control->candidate_valid = true;
    return false;
  }

  control->candidate_delay_ns = requested_delay_ns;
  if (now_ns - control->candidate_since_ns < settle_window_ns)
    return false;

  uint64_t applied_delta = requested_delay_ns > control->applied_delay_ns
      ? requested_delay_ns - control->applied_delay_ns
      : control->applied_delay_ns - requested_delay_ns;
  if (control->has_applied) {
    if (applied_delta < significant_change_ns ||
        now_ns < control->last_applied_ns ||
        now_ns - control->last_applied_ns < update_cooldown_ns)
      return false;
  } else if (requested_delay_ns < significant_change_ns) {
    /* Sub-tolerance skew does not justify flushing the OBS frame queue. */
    return false;
  }

  control->applied_delay_ns = requested_delay_ns;
  control->last_applied_ns = now_ns;
  control->has_applied = true;
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

