#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
  AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
  SwrContext *swr = NULL;
  int ret = swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLTP, 48000,
                                &stereo, AV_SAMPLE_FMT_S16, 48000, 0, NULL);
  if (ret < 0 || swr_init(swr) < 0) {
    fprintf(stderr, "Could not initialize S16-to-FLTP converter\n");
    swr_free(&swr);
    return 1;
  }

  /* Packed input: L=+0.5, R=-0.5 for two samples. */
  int16_t packed[] = {16384, -16384, 8192, -8192};
  const uint8_t *input[] = {(const uint8_t *)packed};
  uint8_t **output = NULL;
  int output_line_size = 0;
  ret = av_samples_alloc_array_and_samples(&output, &output_line_size, 2, 2,
                                            AV_SAMPLE_FMT_FLTP, 0);
  if (ret < 0) {
    swr_free(&swr);
    return 1;
  }

  ret = swr_convert(swr, output, 2, input, 2);
  if (ret != 2) {
    fprintf(stderr, "Expected 2 converted samples, got %d\n", ret);
    av_freep(&output[0]);
    av_freep(&output);
    swr_free(&swr);
    return 1;
  }

  const float *left = (const float *)output[0];
  const float *right = (const float *)output[1];
  int failed = fabsf(left[0] - 0.5f) > 0.0001f ||
               fabsf(left[1] - 0.25f) > 0.0001f ||
               fabsf(right[0] + 0.5f) > 0.0001f ||
               fabsf(right[1] + 0.25f) > 0.0001f;

  av_freep(&output[0]);
  av_freep(&output);
  swr_free(&swr);
  if (failed) {
    fprintf(stderr, "Packed S16 samples did not become the expected planar floats\n");
    return 1;
  }
  puts("S16 packed -> float planar audio conversion passed");
  return 0;
}
