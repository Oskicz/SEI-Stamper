#include "../src/moblin-timecode.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(value, label) do { if (!(value)) { fprintf(stderr, "FAIL: %s\n", label); failures++; } } while (0)

static void put_bits(uint8_t *data, size_t *offset, uint32_t value, unsigned count)
{
  for (unsigned i = 0; i < count; ++i) {
    unsigned bit = (value >> (count - i - 1)) & 1U;
    data[*offset / 8] |= (uint8_t)(bit << (7 - (*offset % 8)));
    ++*offset;
  }
}

static void make_payload(uint8_t payload[6], uint32_t hours, uint32_t minutes,
                         uint32_t seconds, uint32_t frame)
{
  size_t bit = 0;
  memset(payload, 0, 6);
  put_bits(payload, &bit, 1, 2); /* one clock timestamp */
  put_bits(payload, &bit, 1, 1); /* clock_timestamp_flag */
  put_bits(payload, &bit, 1, 1); /* unit_field_based_flag: Moblin */
  put_bits(payload, &bit, 0, 5); /* counting_type */
  put_bits(payload, &bit, 1, 1); /* full_timestamp_flag */
  put_bits(payload, &bit, 0, 1); /* discontinuity_flag */
  put_bits(payload, &bit, 0, 1); /* cnt_dropped_flag */
  put_bits(payload, &bit, frame, 9);
  put_bits(payload, &bit, seconds, 6);
  put_bits(payload, &bit, minutes, 6);
  put_bits(payload, &bit, hours, 5);
  put_bits(payload, &bit, 0, 5); /* time_offset_length */
  put_bits(payload, &bit, 1, 1); /* rbsp trailing bit */
}

static void test_moblin_annexb(void)
{
  uint8_t packet[4 + 2 + 2 + 6 + 1 + 4 + 2 + 1];
  uint8_t payload[6];
  make_payload(payload, 12, 34, 56, 17);
  size_t p = 0;
  const uint8_t prefix[] = {0, 0, 0, 1, 0x4e, 0x01, 0x88, 6};
  memcpy(packet + p, prefix, sizeof(prefix)); p += sizeof(prefix);
  memcpy(packet + p, payload, sizeof(payload)); p += sizeof(payload);
  packet[p++] = 0x80;
  const uint8_t slice[] = {0, 0, 0, 1, 0x26, 0x01, 0xdd};
  memcpy(packet + p, slice, sizeof(slice)); p += sizeof(slice);

  moblin_timecode_t tc = {0};
  CHECK(moblin_extract_timecode_annexb(packet, p, &tc), "find Moblin payloadType 136 in HEVC prefix SEI");
  CHECK(tc.hours == 12 && tc.minutes == 34 && tc.seconds == 56 && tc.frame == 17,
        "decode Moblin time fields and frame number");
}

static void test_reject_invalid_payloads(void)
{
  moblin_timecode_t tc = {0};
  uint8_t payload[6];
  make_payload(payload, 24, 0, 0, 0);
  CHECK(!moblin_parse_timecode_payload(payload, sizeof(payload), &tc), "reject out-of-range hour");
  make_payload(payload, 12, 60, 0, 0);
  CHECK(!moblin_parse_timecode_payload(payload, sizeof(payload), &tc), "reject out-of-range minute");
  make_payload(payload, 12, 0, 60, 0);
  CHECK(!moblin_parse_timecode_payload(payload, sizeof(payload), &tc), "reject out-of-range second");
  make_payload(payload, 12, 0, 0, 0);
  CHECK(!moblin_parse_timecode_payload(payload, 5, &tc), "reject truncated payload");
  payload[0] &= 0x3f; /* numClockTs = 0 */
  CHECK(!moblin_parse_timecode_payload(payload, sizeof(payload), &tc), "reject wrong clock count");
}

static void test_date_resolution(void)
{
  moblin_timecode_t tc = {.hours = 0, .minutes = 0, .seconds = 0, .frame = 0};
  int64_t resolved;
  const int64_t day = 86400LL * 1000000000LL;
  CHECK(moblin_timecode_resolve_unix_ns(&tc, day + 86399LL * 1000000000LL, 30, 1, &resolved),
        "resolve near midnight");
  CHECK(resolved == 2 * day, "choose next day at midnight rollover");
  tc.hours = 23; tc.minutes = 59; tc.seconds = 59;
  CHECK(moblin_timecode_resolve_unix_ns(&tc, 2 * day, 30, 1, &resolved),
        "resolve previous day at midnight rollover");
  CHECK(resolved == 2 * day - 1000000000LL, "choose previous UTC date");
  tc.hours = 12; tc.minutes = 0; tc.seconds = 0;
  CHECK(!moblin_timecode_resolve_unix_ns(&tc, 0, 30, 1, &resolved), "reject ambiguous half-day timestamp");
  tc.hours = 0; tc.minutes = 0; tc.seconds = 0;
  tc.frame = 15;
  CHECK(moblin_timecode_resolve_unix_ns(&tc, 3 * day, 30, 1, &resolved), "resolve frame fraction");
  CHECK(resolved == 3 * day + 500000000LL, "map frame index using nominal stream rate");
  CHECK(!moblin_timecode_resolve_unix_ns(&tc, 0, 0, 1, &resolved), "reject missing frame rate");
}

int main(void)
{
  test_moblin_annexb();
  test_reject_invalid_payloads();
  test_date_resolution();
  if (failures) return 1;
  puts("Moblin HEVC timecode tests passed");
  return 0;
}

