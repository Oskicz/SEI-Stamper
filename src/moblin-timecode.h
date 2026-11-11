#pragma once

/* Small, dependency-free parser for the HEVC time_code SEI emitted by Moblin.
 * The format carries UTC time of day and a frame index, but no calendar date.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct moblin_timecode {
  uint8_t hours;
  uint8_t minutes;
  uint8_t seconds;
  uint16_t frame;
  uint8_t counting_type;
  bool unit_field_based;
} moblin_timecode_t;

/* Time-code frame indexes use the stream's nominal rate, not the observed
 * output cadence. Prefer r_frame_rate so LLB and dropped frames do not change
 * how the time field maps to wall-clock time. */
static inline bool moblin_select_timecode_rate(int nominal_num,
                                                int nominal_den,
                                                int average_num,
                                                int average_den,
                                                int *fps_num,
                                                int *fps_den)
{
  if (!fps_num || !fps_den) return false;
  int num = nominal_num;
  int den = nominal_den;
  if (num <= 0 || den <= 0 || (long double)num / den > 1000.0L) {
    num = average_num;
    den = average_den;
  }
  if (num <= 0 || den <= 0 || (long double)num / den > 1000.0L)
    return false;
  *fps_num = num;
  *fps_den = den;
  return true;
}

typedef struct moblin_bit_reader {
  const uint8_t *data;
  size_t bit_count;
  size_t bit_offset;
} moblin_bit_reader_t;

static inline bool moblin_read_bits(moblin_bit_reader_t *reader, unsigned count,
                                    uint32_t *value)
{
  if (!reader || !value || count > 32 ||
      reader->bit_offset > reader->bit_count ||
      count > reader->bit_count - reader->bit_offset)
    return false;
  uint32_t result = 0;
  for (unsigned i = 0; i < count; ++i) {
    size_t offset = reader->bit_offset++;
    result = (result << 1) |
             ((reader->data[offset / 8] >> (7 - (offset % 8))) & 1U);
  }
  *value = result;
  return true;
}

/* Parse Moblin's emitted subset of HEVC time_code payloadType 136. Reject
 * variants without a full clock or with flags Moblin does not emit so an
 * unrelated or truncated payload cannot be mistaken for a trusted clock.
 */
static inline bool moblin_parse_timecode_payload(const uint8_t *payload,
                                                  size_t size,
                                                  moblin_timecode_t *out)
{
  if (!payload || !out || size < 6 || size > 64)
    return false;
  moblin_bit_reader_t r = {payload, size * 8, 0};
  uint32_t clocks, flag, unit_field, counting, full, discontinuity, dropped;
  uint32_t frame, seconds, minutes, hours, offset_length;
  if (!moblin_read_bits(&r, 2, &clocks) || clocks != 1 ||
      !moblin_read_bits(&r, 1, &flag) || flag != 1 ||
      !moblin_read_bits(&r, 1, &unit_field) || unit_field != 1 ||
      !moblin_read_bits(&r, 5, &counting) || counting != 0 ||
      !moblin_read_bits(&r, 1, &full) || full != 1 ||
      !moblin_read_bits(&r, 1, &discontinuity) || discontinuity != 0 ||
      !moblin_read_bits(&r, 1, &dropped) || dropped != 0 ||
      !moblin_read_bits(&r, 9, &frame) ||
      !moblin_read_bits(&r, 6, &seconds) ||
      !moblin_read_bits(&r, 6, &minutes) ||
      !moblin_read_bits(&r, 5, &hours) ||
      !moblin_read_bits(&r, 5, &offset_length) || offset_length != 0 ||
      seconds > 59 || minutes > 59 || hours > 23)
    return false;
  out->hours = (uint8_t)hours;
  out->minutes = (uint8_t)minutes;
  out->seconds = (uint8_t)seconds;
  out->frame = (uint16_t)frame;
  out->counting_type = (uint8_t)counting;
  out->unit_field_based = true;
  return true;
}

static inline bool moblin_find_start_code(const uint8_t *data, size_t size,
                                          size_t from, size_t *start,
                                          size_t *prefix)
{
  for (size_t i = from; i + 3 <= size; ++i) {
    if (data[i] == 0 && data[i + 1] == 0) {
      if (data[i + 2] == 1) {
        *start = i; *prefix = 3; return true;
      }
      if (i + 4 <= size && data[i + 2] == 0 && data[i + 3] == 1) {
        *start = i; *prefix = 4; return true;
      }
    }
  }
  return false;
}

static inline bool moblin_parse_sei_rbsp(const uint8_t *rbsp, size_t size,
                                         moblin_timecode_t *out)
{
  size_t pos = 0;
  while (pos < size) {
    size_t type = 0, payload_size = 0;
    uint8_t byte;
    do {
      if (pos >= size || type > SIZE_MAX - 255) return false;
      byte = rbsp[pos++]; type += byte;
    } while (byte == 0xff);
    do {
      if (pos >= size || payload_size > SIZE_MAX - 255) return false;
      byte = rbsp[pos++]; payload_size += byte;
    } while (byte == 0xff);
    if (payload_size > size - pos) return false;
    if (type == 136 && moblin_parse_timecode_payload(rbsp + pos, payload_size, out))
      return true;
    pos += payload_size;
    /* rbsp_trailing_bits: a single 1 followed by zero padding. */
    if (pos == size || (pos + 1 == size && rbsp[pos] == 0x80)) break;
  }
  return false;
}

/* Locate payloadType 136 in Annex-B HEVC prefix/suffix SEI NALs. This is
 * deliberately bounded; oversized SEI NALs are ignored and normal decode
 * continues. */
static inline bool moblin_extract_timecode_annexb(const uint8_t *packet,
                                                   size_t size,
                                                   moblin_timecode_t *out)
{
  if (!packet || !out) return false;
  size_t nal_start, prefix;
  if (!moblin_find_start_code(packet, size, 0, &nal_start, &prefix)) return false;
  while (nal_start < size) {
    size_t header = nal_start + prefix;
    size_t next_start, next_prefix;
    bool has_next = moblin_find_start_code(packet, size, header, &next_start, &next_prefix);
    size_t end = has_next ? next_start : size;
    if (header + 2 <= end) {
      uint8_t nal_type = (packet[header] >> 1) & 0x3f;
      if (nal_type == 39 || nal_type == 40) {
        uint8_t rbsp[4096]; size_t rbsp_size = 0; unsigned zeros = 0;
        for (size_t i = header + 2; i < end; ++i) {
          uint8_t b = packet[i];
          if (zeros >= 2 && b == 3) { zeros = 0; continue; }
          if (rbsp_size == sizeof(rbsp)) { rbsp_size = 0; break; }
          rbsp[rbsp_size++] = b;
          zeros = b == 0 ? zeros + 1 : 0;
        }
        if (rbsp_size && moblin_parse_sei_rbsp(rbsp, rbsp_size, out)) return true;
      }
    }
    if (!has_next) break;
    nal_start = next_start; prefix = next_prefix;
  }
  return false;
}

/* Moblin timecode has no date. Resolve it against trusted current UTC by
 * selecting the nearest UTC day; reject the exactly-half-day ambiguity. */
static inline bool moblin_timecode_resolve_unix_ns(const moblin_timecode_t *tc,
                                                   int64_t reference_unix_ns,
                                                   int fps_num, int fps_den,
                                                   int64_t *unix_ns)
{
  const int64_t second_ns = 1000000000LL;
  const int64_t day_ns = 86400LL * second_ns;
  if (!tc || !unix_ns || fps_num <= 0 || fps_den <= 0 ||
      (long double)fps_num / fps_den > 1000.0L || tc->hours > 23 ||
      tc->minutes > 59 || tc->seconds > 59)
    return false;
  long double frame_ns_ld = (long double)tc->frame * fps_den * second_ns / fps_num;
  if (frame_ns_ld < 0 || frame_ns_ld >= second_ns) return false;
  int64_t tod = ((int64_t)tc->hours * 3600 + (int64_t)tc->minutes * 60 + tc->seconds) * second_ns + (int64_t)(frame_ns_ld + 0.5L);
  int64_t day = reference_unix_ns / day_ns;
  if (reference_unix_ns < 0 && reference_unix_ns % day_ns) --day;
  int64_t best = 0, best_delta = INT64_MAX;
  for (int d = -1; d <= 1; ++d) {
    int64_t candidate = (day + d) * day_ns + tod;
    int64_t delta = candidate >= reference_unix_ns ? candidate - reference_unix_ns : reference_unix_ns - candidate;
    if (delta < best_delta) { best = candidate; best_delta = delta; }
  }
  if (best_delta >= day_ns / 2) return false;
  *unix_ns = best;
  return true;
}

