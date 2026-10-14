#include "../src/multicam-status.h"
#include <stdio.h>

int main(void)
{
  int key;
  multicam_status_register(&key, "Private source name");
  multicam_status_set_connection(&key, true, "hevc", true, 1920, 1080);
  multicam_status_add_packet(&key, 1000, 1000000000ULL);
  multicam_status_add_packet(&key, 1000, 2000000000ULL);
  multicam_source_status_t measured = {0};
  if (multicam_status_snapshot(&measured, 1) != 1 ||
      measured.rx_bitrate_bps != 16000 || measured.rx_bitrate_5s_bps != 16000 || !measured.has_audio ||
      measured.connected == false) return 7;
  multicam_status_set_connection(&key, false, NULL, false, 0, 0);
  if (multicam_status_snapshot(&measured, 1) != 1 || measured.connected ||
      measured.rx_bitrate_bps || measured.last_video_mono_ns) return 8;
  multicam_status_unregister(&key);

  const uint64_t now = 5000000000ULL;
  multicam_source_status_t cameras[2] = {
    {.connected = true, .ntp_fresh = true, .sync_method = MULTICAM_SYNC_SEI_STAMPER_NTP,
     .last_video_mono_ns = now, .last_audio_mono_ns = now, .has_audio = true,
     .last_packet_mono_ns = now,
     .last_absolute_unix_ns = 100000000000ULL, .rx_bitrate_bps = 1000000},
    {.connected = true, .ntp_fresh = true, .sync_method = MULTICAM_SYNC_MOBLIN_TIMECODE,
     .last_packet_mono_ns = now, .last_video_mono_ns = now,
     .last_absolute_unix_ns = 100015000000ULL,
     .rx_bitrate_bps = 800000}
  };
  uint64_t spread = 0;
  if (!multicam_status_source_ready(&cameras[0], now)) return 1;
  if (multicam_status_source_ready(&cameras[1], now) == false) return 2;
  if (!multicam_status_group_aligned(cameras, 2, now, 16666667ULL, &spread) ||
      spread != 15000000ULL) return 3;
  if (multicam_status_group_aligned(cameras, 2, now, 10000000ULL, NULL)) return 4;
  cameras[1].sync_method = MULTICAM_SYNC_PTS_FALLBACK;
  if (multicam_status_source_ready(&cameras[1], now)) return 5;
  cameras[1].sync_method = MULTICAM_SYNC_MOBLIN_TIMECODE;
  cameras[1].last_video_mono_ns = 1;
  if (multicam_status_source_ready(&cameras[1], now)) return 6;
  puts("Multicam status and conservative preflight tests passed");
  return 0;
}

