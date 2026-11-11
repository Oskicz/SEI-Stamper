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


#define RECEIVER_AUDIO_JITTER_CUSHION_NS 120000000ULL
#define RECEIVER_MAX_GROUP_RELATIVE_WAIT_NS 5000000000ULL
#define RECEIVER_MAX_GROUP_PLAYOUT_DELAY_NS 10000000000ULL

/* Limit the extra wait imposed on the fastest camera while allowing the
 * common output clock to cover real multi-second transport latency. */
static inline uint64_t receiver_group_playout_target_ns(
    uint64_t maximum_input_age_ns, uint64_t minimum_input_age_ns,
    bool any_audio)
{
  uint64_t relative_max = minimum_input_age_ns >
                                  UINT64_MAX - RECEIVER_MAX_GROUP_RELATIVE_WAIT_NS
                              ? UINT64_MAX
                              : minimum_input_age_ns +
                                    RECEIVER_MAX_GROUP_RELATIVE_WAIT_NS;
  uint64_t target_ns = maximum_input_age_ns < relative_max
                           ? maximum_input_age_ns
                           : relative_max;
  if (any_audio)
    target_ns = target_ns > UINT64_MAX - RECEIVER_AUDIO_JITTER_CUSHION_NS
                    ? UINT64_MAX
                    : target_ns + RECEIVER_AUDIO_JITTER_CUSHION_NS;
  return target_ns > RECEIVER_MAX_GROUP_PLAYOUT_DELAY_NS
             ? RECEIVER_MAX_GROUP_PLAYOUT_DELAY_NS : target_ns;
}
#define RECEIVER_SYNC_SPEED_DEADBAND_NS 1000000ULL
#define RECEIVER_SYNC_SPEED_SOFT_ERROR_NS 250000000ULL
#define RECEIVER_SYNC_SPEED_MAX_ERROR_NS 1500000000ULL
#define RECEIVER_SYNC_SPEED_SOFT_CORRECTION 0.04
#define RECEIVER_SYNC_SPEED_MAX_CORRECTION 0.12
#define RECEIVER_SYNC_SPEED_MIN (1.0 - RECEIVER_SYNC_SPEED_MAX_CORRECTION)
#define RECEIVER_SYNC_SPEED_MAX (1.0 + RECEIVER_SYNC_SPEED_MAX_CORRECTION)

typedef struct receiver_group_delay_control {
  bool initialized;
  uint64_t applied_ns;
  uint64_t last_update_ns;
  double speed;
} receiver_group_delay_control_t;

typedef struct receiver_audio_output_clock {
  bool initialized;
  uint64_t anchor_ns;
  uint64_t samples;
  uint32_t sample_rate;
} receiver_audio_output_clock_t;

/* Positive skew speeds media up; negative skew slows it down. Map a small
 * residual to at most 4% correction and large errors to at most 12%. */
static inline double receiver_sync_speed_for_skew_ns(int64_t skew_ns)
{
  uint64_t magnitude = skew_ns < 0
      ? (uint64_t)(-(skew_ns + 1)) + 1ULL : (uint64_t)skew_ns;
  if (magnitude <= RECEIVER_SYNC_SPEED_DEADBAND_NS)
    return 1.0;
  double correction;
  if (magnitude <= RECEIVER_SYNC_SPEED_SOFT_ERROR_NS) {
    correction = RECEIVER_SYNC_SPEED_SOFT_CORRECTION *
                 (double)(magnitude - RECEIVER_SYNC_SPEED_DEADBAND_NS) /
                 (double)(RECEIVER_SYNC_SPEED_SOFT_ERROR_NS -
                          RECEIVER_SYNC_SPEED_DEADBAND_NS);
  } else {
    uint64_t extra = magnitude - RECEIVER_SYNC_SPEED_SOFT_ERROR_NS;
    uint64_t range = RECEIVER_SYNC_SPEED_MAX_ERROR_NS -
                     RECEIVER_SYNC_SPEED_SOFT_ERROR_NS;
    if (extra > range) extra = range;
    correction = RECEIVER_SYNC_SPEED_SOFT_CORRECTION +
                 (RECEIVER_SYNC_SPEED_MAX_CORRECTION -
                  RECEIVER_SYNC_SPEED_SOFT_CORRECTION) *
                     (double)extra / (double)range;
  }
  return skew_ns > 0 ? 1.0 + correction : 1.0 - correction;
}

/* Slew the shared delay instead of jumping timestamps. Since every receiver
 * uses this same clock, A/V and cameras keep one common presentation timeline. */
static inline uint64_t receiver_group_delay_step(
    receiver_group_delay_control_t *control, uint64_t target_ns,
    uint64_t now_ns, double *playout_speed)
{
  if (!control) {
    if (playout_speed) *playout_speed = 1.0;
    return target_ns;
  }
  if (!control->initialized) {
    control->initialized = true;
    control->applied_ns = target_ns;
    control->last_update_ns = now_ns;
    control->speed = 1.0;
    if (playout_speed) *playout_speed = control->speed;
    return control->applied_ns;
  }
  if (now_ns < control->last_update_ns) {
    control->last_update_ns = now_ns;
    control->speed = 1.0;
    if (playout_speed) *playout_speed = control->speed;
    return control->applied_ns;
  }

  uint64_t elapsed_ns = now_ns - control->last_update_ns;
  control->last_update_ns = now_ns;
  if (elapsed_ns > 100000000ULL) elapsed_ns = 100000000ULL;
  uint64_t magnitude = target_ns > control->applied_ns
                           ? target_ns - control->applied_ns
                           : control->applied_ns - target_ns;
  double alpha = (double)elapsed_ns / 250000000.0;
  if (alpha > 1.0) alpha = 1.0;
  if (magnitude <= RECEIVER_SYNC_SPEED_DEADBAND_NS) {
    control->applied_ns = target_ns;
    control->speed = 1.0;
  } else {
    int64_t signed_skew = target_ns < control->applied_ns
        ? (int64_t)magnitude : -(int64_t)magnitude;
    double target_speed = receiver_sync_speed_for_skew_ns(signed_skew);
    control->speed += (target_speed - control->speed) * alpha;
    if (control->speed < RECEIVER_SYNC_SPEED_MIN)
      control->speed = RECEIVER_SYNC_SPEED_MIN;
    if (control->speed > RECEIVER_SYNC_SPEED_MAX)
      control->speed = RECEIVER_SYNC_SPEED_MAX;
    double delay_rate = 1.0 / control->speed - 1.0;
    if (delay_rate < 0.0) delay_rate = -delay_rate;
    long double desired_change = (long double)elapsed_ns * delay_rate;
    uint64_t change_ns = (uint64_t)(desired_change + 0.5L);
    if (change_ns >= magnitude) {
      control->applied_ns = target_ns;
      control->speed = 1.0;
    } else if (target_ns > control->applied_ns) {
      control->applied_ns += change_ns;
    } else {
      control->applied_ns -= change_ns;
    }
  }
  if (playout_speed) *playout_speed = control->speed;
  return control->applied_ns;
}

static inline int receiver_sync_output_samples(int input_samples, double speed,
                                               double *carry)
{
  if (input_samples <= 0) return 0;
  if (speed < RECEIVER_SYNC_SPEED_MIN || speed > RECEIVER_SYNC_SPEED_MAX)
    speed = 1.0;
  double desired = (double)input_samples / speed + (carry ? *carry : 0.0);
  int output = (int)(desired + 0.5);
  if (output < 1) output = 1;
  if (carry) *carry = desired - (double)output;
  return output;
}

static inline void receiver_audio_output_clock_start(
    receiver_audio_output_clock_t *clock, uint64_t anchor_ns,
    uint32_t sample_rate)
{
  if (!clock) return;
  clock->initialized = sample_rate > 0;
  clock->anchor_ns = anchor_ns;
  clock->samples = 0;
  clock->sample_rate = sample_rate;
}

static inline uint64_t receiver_audio_output_clock_next_ns(
    const receiver_audio_output_clock_t *clock)
{
  if (!clock || !clock->initialized || !clock->sample_rate) return 0;
  uint64_t delta = (uint64_t)((long double)clock->samples *
                              1000000000.0L / clock->sample_rate);
  return delta > UINT64_MAX - clock->anchor_ns
             ? UINT64_MAX : clock->anchor_ns + delta;
}

static inline void receiver_audio_output_clock_advance(
    receiver_audio_output_clock_t *clock, uint32_t output_samples)
{
  if (clock && clock->initialized &&
      output_samples <= UINT64_MAX - clock->samples)
    clock->samples += output_samples;
}

static inline uint64_t receiver_add_playout_latency_ns(uint64_t timestamp_ns,
                                                        uint64_t latency_ns)
{
  return latency_ns > UINT64_MAX - timestamp_ns ? UINT64_MAX
                                                 : timestamp_ns + latency_ns;
}

/* Map an independent audio-stream PTS epoch onto the video capture clock.
 * Monotonic observation times compensate for packets decoded at different
 * points in the same live stream. */
static inline bool receiver_audio_pts_anchor_offset_ns(
    int64_t video_capture_ns, uint64_t video_observed_mono_ns,
    int64_t audio_pts_ns, uint64_t audio_observed_mono_ns,
    int64_t *audio_offset_ns)
{
  if (!audio_offset_ns) return false;
  int64_t projected_video_capture_ns = video_capture_ns;
  if (audio_observed_mono_ns >= video_observed_mono_ns) {
    uint64_t elapsed = audio_observed_mono_ns - video_observed_mono_ns;
    if (elapsed > (uint64_t)INT64_MAX ||
        projected_video_capture_ns > INT64_MAX - (int64_t)elapsed)
      return false;
    projected_video_capture_ns += (int64_t)elapsed;
  } else {
    uint64_t elapsed = video_observed_mono_ns - audio_observed_mono_ns;
    if (elapsed > (uint64_t)INT64_MAX ||
        projected_video_capture_ns < INT64_MIN + (int64_t)elapsed)
      return false;
    projected_video_capture_ns -= (int64_t)elapsed;
  }
  if ((audio_pts_ns < 0 &&
       projected_video_capture_ns > INT64_MAX + audio_pts_ns) ||
      (audio_pts_ns > 0 &&
       projected_video_capture_ns < INT64_MIN + audio_pts_ns))
    return false;
  *audio_offset_ns = projected_video_capture_ns - audio_pts_ns;
  return true;
}

static inline bool receiver_add_signed_ns(int64_t timestamp_ns,
                                           int64_t offset_ns,
                                           int64_t *result_ns)
{
  if (!result_ns ||
      (offset_ns > 0 && timestamp_ns > INT64_MAX - offset_ns) ||
      (offset_ns < 0 && timestamp_ns < INT64_MIN - offset_ns))
    return false;
  *result_ns = timestamp_ns + offset_ns;
  return true;
}

static inline int64_t receiver_smooth_audio_pts_offset_ns(int64_t current_ns,
                                                           int64_t target_ns)
{
  long double next = (long double)current_ns +
                     ((long double)target_ns - (long double)current_ns) / 16.0L;
  if (next > (long double)INT64_MAX) return INT64_MAX;
  if (next < (long double)INT64_MIN) return INT64_MIN;
  return (int64_t)(next >= 0 ? next + 0.5L : next - 0.5L);
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

#define RECEIVER_AUDIO_DECODER_ERROR_BURST 3U
#define RECEIVER_AUDIO_DECODER_FLUSH_COOLDOWN_NS 1000000000ULL

static inline bool receiver_audio_decoder_recovery_due(uint32_t consecutive_errors,
                                                       uint64_t now_ns,
                                                       uint64_t last_flush_ns)
{
  if (consecutive_errors < RECEIVER_AUDIO_DECODER_ERROR_BURST)
    return false;
  if (!last_flush_ns)
    return true;
  return now_ns >= last_flush_ns &&
         now_ns - last_flush_ns >= RECEIVER_AUDIO_DECODER_FLUSH_COOLDOWN_NS;
}

static inline uint32_t receiver_ntp_max_age_seconds(uint32_t sync_interval_ms)
{
  uint64_t interval_seconds = ((uint64_t)sync_interval_ms + 999ULL) / 1000ULL;
  uint64_t max_age = interval_seconds * 3ULL;
  if (max_age < 120ULL) max_age = 120ULL;
  if (max_age > 3600ULL) max_age = 3600ULL;
  return (uint32_t)max_age;
}

