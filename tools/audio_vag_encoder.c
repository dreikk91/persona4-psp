/* Offline PSX ADPCM encoder for SAS streaming, built into helpers/p4meshopt.dll.
   No game code runs this encoder. Output is byte-identical to the former audio_vag_encoder.cs. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const int C1[5] = {0, 60, 115, 98, 122};
static const int C2[5] = {0, 0, -52, -55, -60};

static int clamp16(int x) { return x < -32768 ? -32768 : x > 32767 ? 32767 : x; }

static void encode_block(const int16_t *pcm, int stride, int count, uint8_t *dst, int *h1, int *h2, int reset)
{
    long long best = INT64_MAX;
    int best1 = 0, best2 = 0;
    for (int filter = 0; filter < (reset ? 1 : 5); ++filter) {
        int max_error = 0, a = *h1, b = *h2;
        for (int i = 0; i < 28; ++i) {
            int value = i < count ? pcm[i * stride] : 0;
            int e = abs(value - ((a * C1[filter] + b * C2[filter] + 32) >> 6));
            if (e > max_error) max_error = e;
            b = a; a = value;
        }
        int shift = 12;
        while (shift > 0 && max_error > (7 << (12 - shift))) --shift;
        /* Try neighbouring scales as predictor feedback changes the residuals. */
        for (int s = shift > 0 ? shift - 1 : 0; s <= (shift < 12 ? shift + 1 : 12); ++s) {
            uint8_t candidate[16] = {(uint8_t)((filter << 4) | s)};
            long long error = 0;
            int step = 1 << (12 - s);
            a = *h1; b = *h2;
            for (int i = 0; i < 28; ++i) {
                int value = i < count ? pcm[i * stride] : 0;
                int prediction = (a * C1[filter] + b * C2[filter] + 32) >> 6;
                /* rint rounds half to even, as C# Math.Round does. */
                int q = (int)rint((value - prediction) / (double)step);
                q = q < -8 ? -8 : q > 7 ? 7 : q;
                int reconstructed = clamp16(prediction + q * step);
                long long d = value - reconstructed; error += d * d;
                candidate[2 + i / 2] |= (uint8_t)((q & 15) << ((i & 1) * 4));
                b = a; a = reconstructed;
            }
            if (error < best) {
                best = error; best1 = a; best2 = b;
                memcpy(dst, candidate, 16);
            }
        }
    }
    *h1 = best1; *h2 = best2;
}

/* Interleaved s16 PCM -> 16-byte blocks, channel-interleaved per 28-sample block.
   dst holds ceil(frames / 28) * channels * 16 bytes. */
#ifdef __cplusplus
extern "C"
#endif
__declspec(dllexport) int p4_vag_encode(const int16_t *pcm, size_t frames, int channels, long long loop_block, uint8_t *dst)
{
    if (channels < 1 || channels > 2) return -1;
    int h1[2] = {0, 0}, h2[2] = {0, 0};
    for (long long block = 0; frames > 0; ++block) {
        int n = frames < 28 ? (int)frames : 28;
        for (int c = 0; c < channels; ++c) {
            /* Independent 256-block chunks permit source loops and ring refills. */
            encode_block(pcm + c, channels, n, dst, &h1[c], &h2[c], block % 256 == 0 || block == loop_block);
            dst += 16;
        }
        pcm += 28 * channels;
        frames -= n;
    }
    return 0;
}
