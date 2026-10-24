/* The game core through its C interface only (include/m2core.h), as a front
 * end uses it: load the ROM set, run FRAMES frames with no input, print the
 * last screen's hash (the same FNV-1a as m2run's "last screen hash") and how
 * much sound came out. C, so the header stays C.
 *
 *   m2coreshot ROMSET.zip FRAMES
 */
#include "m2core.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define M2_ENTRY_(set) m2core_api_##set
#define M2_ENTRY(set) M2_ENTRY_(set)

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: m2coreshot ROMSET.zip FRAMES\n");
        return 2;
    }
    const m2core_api *api = M2_ENTRY(M2_ROMSET_ID)();
    if (api->version != M2CORE_API_VERSION) {
        fprintf(stderr, "m2coreshot: API version %u, expected %d\n", api->version, M2CORE_API_VERSION);
        return 1;
    }
    char err[512];
    m2core *core = api->create(argv[1], NULL, err, sizeof err);
    if (!core) {
        fprintf(stderr, "m2coreshot: %s\n", err);
        return 1;
    }
    const long frames = atol(argv[2]);
    static float buf[2 * 4096];
    size_t sound[2] = {0, 0};
    m2core_input in = {0};
    for (long f = 0; f < frames; ++f) {
        api->run_frame(core, &in);
        for (int s = 0; s < 2; ++s) {
            size_t n;
            while ((n = api->audio_read(core, s, buf, 4096)) > 0) sound[s] += n;
        }
    }
    int w, h;
    const uint32_t *px = api->screen(core, &w, &h);
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (long i = 0; i < (long)w * h; ++i)
        for (int b = 0; b < 4; ++b) {
            hash ^= (px[i] >> (8 * b)) & 0xff;
            hash *= 0x100000001b3ULL;
        }
    printf("%s (%s): %ld frames, %dx%d, last screen hash %016" PRIx64 "\n", api->title, api->romset, frames, w, h, hash);
    printf("  sound: %zu frames at %.0f Hz, %zu at %.0f Hz\n", sound[0], api->audio_rate(core, 0), sound[1], api->audio_rate(core, 1));
    api->destroy(core);
    return 0;
}
