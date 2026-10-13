#pragma once

#include <stdint.h>
#include <stdbool.h>

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
