#include "../src/multicam-status.h"
#include <stdio.h>

int main(void)
{
  if (multicam_status_sync_quality(39999999ULL) != MULTICAM_SYNC_QUALITY_EXCELLENT ||
      multicam_status_sync_quality(40000000ULL) != MULTICAM_SYNC_QUALITY_GOOD ||
      multicam_status_sync_quality(99999999ULL) != MULTICAM_SYNC_QUALITY_GOOD ||
      multicam_status_sync_quality(100000000ULL) != MULTICAM_SYNC_QUALITY_WARNING ||
      multicam_status_sync_quality(249999999ULL) != MULTICAM_SYNC_QUALITY_WARNING ||
      multicam_status_sync_quality(250000000ULL) != MULTICAM_SYNC_QUALITY_BAD) return 11;
  if (multicam_status_playout_delay(3900000000ULL, 2330000000ULL) !=
          1570000000ULL ||
      multicam_status_playout_delay(2330000000ULL, 3900000000ULL) != 0)
    return 12;

  int key;
  multicam_status_register(&key, "Private source name");
  multicam_status_set_connection(&key, true, "hevc", true, 1920, 1080);
  multicam_status_add_packet(&key, 1000, 1000000000ULL);
  multicam_status_add_packet(&key, 1000, 2000000000ULL);
  multicam_status_video(&key, MULTICAM_SYNC_MOBLIN_TIMECODE, true,
                        900000000000LL, 550000000ULL, 1920, 1080, 0,
                        2000000000ULL);
  multicam_status_set_sync_latency(&key, 550000000ULL);
  multicam_source_status_t measured = {0};
  if (multicam_status_snapshot(&measured, 1) != 1 ||
      measured.rx_bitrate_bps != 16000 || measured.rx_bitrate_5s_bps != 16000 || !measured.has_audio ||
      measured.connected == false || measured.observed_latency_ns != 550000000ULL ||
      multicam_status_group_max_latency_ns(2000000000ULL) != 550000000ULL) return 7;
  multicam_status_set_playout_delay(&key, 1250000000ULL);
  if (multicam_status_snapshot(&measured, 1) != 1 ||
      measured.playout_delay_ns != 1250000000ULL) return 14;
  multicam_status_set_audio_decode_errors(&key, 3);
  if (multicam_status_snapshot(&measured, 1) != 1 ||
      measured.audio_decode_errors != 3) return 9;
  multicam_status_set_connection(&key, false, NULL, false, 0, 0);
  if (multicam_status_snapshot(&measured, 1) != 1 || measured.connected ||
      measured.rx_bitrate_bps || measured.last_video_mono_ns) return 8;
  multicam_status_unregister(&key);

  const uint64_t now = 5000000000ULL;
  multicam_source_status_t cameras[2] = {
    {.name = "OBS Camera", .connected = true, .ntp_fresh = true,
     .sync_method = MULTICAM_SYNC_SEI_STAMPER_NTP,
     .last_video_mono_ns = now, .last_audio_mono_ns = now, .has_audio = true,
     .last_packet_mono_ns = now, .last_absolute_unix_ns = 100000000000ULL,
     .sync_latency_ns = 3900000000ULL, .rx_bitrate_bps = 1000000},
    {.name = "Moblin Camera", .connected = true, .ntp_fresh = true,
     .sync_method = MULTICAM_SYNC_MOBLIN_TIMECODE,
     .last_packet_mono_ns = now, .last_video_mono_ns = now,
     .last_absolute_unix_ns = 101540000000ULL,
     .sync_latency_ns = 2359000000ULL, .rx_bitrate_bps = 800000}
  };
  uint64_t spread = 0;
  if (!multicam_status_source_ready(&cameras[0], now)) return 1;
  if (!multicam_status_source_ready(&cameras[1], now)) return 2;
  if (multicam_status_group_aligned(cameras, 2, now, 16666667ULL, &spread) ||
      spread != 1540000000ULL) return 3;
  if (multicam_status_group_aligned(cameras, 2, now, 10000000ULL, NULL)) return 4;
  cameras[0].playout_delay_ns = 1540000000ULL;
  if (!multicam_status_group_aligned(cameras, 2, now, 16666667ULL, &spread) ||
      spread != 0) return 13;
  const char *selected_names[] = {"Moblin Camera"};
  if (multicam_status_selected_scene_delay_ns(cameras, 2, selected_names, 1,
                                               now) != 2359000000ULL) return 15;
  const char *unmatched_names[] = {"Scene without receiver"};
  if (multicam_status_selected_scene_delay_ns(cameras, 2, unmatched_names, 1,
                                               now) != 0) return 16;
  cameras[1].connected = false;
  if (multicam_status_selected_scene_delay_ns(cameras, 2, selected_names, 1,
                                               now) != 0) return 17;
  cameras[1].connected = true;
  cameras[0].playout_delay_ns = 0;
  cameras[1].sync_method = MULTICAM_SYNC_PTS_FALLBACK;
  if (multicam_status_source_ready(&cameras[1], now)) return 5;
  cameras[1].sync_method = MULTICAM_SYNC_MOBLIN_TIMECODE;
  cameras[1].last_video_mono_ns = 1;
  if (multicam_status_source_ready(&cameras[1], now)) return 6;
  spread = 0;
  if (multicam_status_group_aligned(cameras, 2, now, 16666667ULL, &spread) ||
      spread != 1540000000ULL) return 10;
  puts("Multicam status and conservative preflight tests passed");
  return 0;
}

