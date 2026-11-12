# Multicam stability settings

## Receiver starting point

- Enable NTP sync on every receiver.
- Use UDP port `123` and a `10,000 ms` sync interval. Keep the drift threshold at `50 ms` initially.
- Keep the audio offset at `0 ms` until checking playback. The diagnostic panel does not verify lip sync.
- Use automatic codec detection. Prefer software decode for the first stability test; enable hardware decode only after it is stable on the exact GPU/driver used for the event.
- SRT receive latency is transport buffering, not an NTP correction. Start with the server/Moblin SRTLA latency that has already been tested on that route. Do not change it during an event as a way to correct a timestamp offset.

## Sender clocks

The OBS sender and Moblin device need clocks synchronized to UTC. Using the same NTP host on every device reduces uncertainty, but is not mandatory if each device's clock is independently accurate. Keep the sender's NTP metadata enabled. Moblin HEVC timecode is mapped against the receiver's current NTP date; it must contain valid timecode SEI and the receiver must have a fresh NTP reading.

## What the supplied diagnostics mean

The first report measured input ages of about `889 ms` and `4,403 ms`, a difference of about `3.52 s`. This is a real timestamp/transport-path age difference. A receiver-side NTP polling interval cannot remove it. The second report's `0` spread was emitted while one camera was not ready; the report now computes the measured spread even when another readiness check fails.

At 60 fps, one video frame is about `16.67 ms`. Timestamp precision alone cannot guarantee `10 ms` image alignment, and a live playback test is needed to verify A/V behavior. This build resets the mapping after a PTS rewind and clears stale video; it does not add a multi-second group playout buffer or speed up a late feed. A measured multi-second age gap therefore remains a real transport/capture delay. Do not treat a green readiness panel as proof of audible lip sync.
