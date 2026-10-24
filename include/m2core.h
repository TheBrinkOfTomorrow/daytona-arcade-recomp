/*
 * The game core's C interface: one recompiled game (one ROM set) as a
 * library a front end drives a frame at a time. Built as `m2core` (CMake
 * M2_CORE): a shared library, or a framework on Apple platforms, that
 * exports one symbol, m2core_api_<romset>() (m2core_api_daytona93,
 * m2core_api_daytona, m2core_api_srallyc), and hides everything else, so
 * several games load into one process without clashing.
 *
 * Threading: a core is used from one thread at a time. Nothing blocks:
 * run_frame returns once the board has composed the next screen.
 */
#ifndef M2CORE_H
#define M2CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M2CORE_API_VERSION 1

typedef struct m2core m2core;

/* Buttons (m2core_input.buttons). */
enum {
    M2CORE_COIN = 1u << 0,
    M2CORE_START = 1u << 1,
    M2CORE_TEST = 1u << 2,    /* the board's test switch: test mode */
    M2CORE_SERVICE = 1u << 3, /* service: a credit, or the cursor in test mode */
    M2CORE_VIEW1 = 1u << 4,   /* Daytona VR1-4; Sega Rally VIEW (VIEW1 only) */
    M2CORE_VIEW2 = 1u << 5,
    M2CORE_VIEW3 = 1u << 6,
    M2CORE_VIEW4 = 1u << 7
};

typedef struct m2core_input {
    float steer;      /* -1 full left .. +1 full right */
    float accel;      /* 0 .. 1 */
    float brake;      /* 0 .. 1 */
    float handbrake;  /* 0 .. 1 (Sega Rally; ignored by Daytona) */
    int gear;         /* 0 neutral, 1-4 */
    uint32_t buttons; /* M2CORE_* held this frame */
} m2core_input;

/* Link play's state (link_state). */
enum {
    M2CORE_LINK_OFF = 0,     /* no link, or the game has not started it */
    M2CORE_LINK_WAITING = 1, /* looking for the other cabinets */
    M2CORE_LINK_UP = 2,
    M2CORE_LINK_LOST = 3     /* reset (destroy and create) to try again */
};

typedef struct m2core_api {
    uint32_t version; /* M2CORE_API_VERSION */
    const char *romset; /* "daytona93", "daytona", "srallyc" */
    const char *title;  /* "Daytona USA", "Sega Rally Championship" */
    double frame_hz;    /* the board's frame rate (57.52 Hz) */
    int width, height;  /* the original screen: 496 x 384 */

    /* rom_path: the user's MAME ROM set (.zip or .7z), or a folder of the
     * images m2import made from it. save_dir: where the
     * game's settings EEPROM and backup RAM are kept (created by the caller;
     * NULL keeps nothing). On failure returns NULL and writes why to err. */
    m2core *(*create)(const char *rom_path, const char *save_dir, char *err, size_t err_size);
    /* Saves the settings (as save does) and frees the core. */
    void (*destroy)(m2core *core);

    /* Run until the next screen is composed. input: this frame's controls. */
    void (*run_frame)(m2core *core, const m2core_input *input);

    /* The screen: *width x 384 pixels, 0xAARRGGBB words (BGRA bytes on a
     * little-endian host), rows packed. Valid until the next run_frame. */
    const uint32_t *(*screen)(m2core *core, int *width, int *height);

    /* Sound since the last call: the board's outputs as interleaved stereo
     * floats, each at its own rate (stream 0 and 1; Sega Rally's SCSP is
     * stream 0 and stream 1 is empty). Mix them at equal gain. Writes at
     * most max_frames frames and returns how many; the rest is kept for the
     * next call. */
    double (*audio_rate)(m2core *core, int stream);
    size_t (*audio_read)(m2core *core, int stream, float *out, size_t max_frames);

    /* Writes the settings EEPROM and backup RAM to save_dir now. */
    void (*save)(m2core *core);

    /* Link play over TCP: listen on listen_port for the cabinet before this
     * one, connect to next ("host:port"). Call before the first run_frame.
     * framesync holds every cabinet to the master's frame. Returns 0, or -1
     * with the reason in err. Which cabinet this is comes from the game's own
     * test-mode setting (Daytona: GAME SYSTEM > LINK ID; Sega Rally: GAME
     * ASSIGNMENTS > LINK TYPE). */
    int (*link_start)(m2core *core, int listen_port, const char *next, int framesync, char *err, size_t err_size);
    /* M2CORE_LINK_*; *id and *count (may be NULL) once it is up. */
    int (*link_state)(m2core *core, int *id, int *count);
} m2core_api;

#if defined(_WIN32)
#ifdef M2CORE_BUILD
#define M2CORE_EXPORT __declspec(dllexport)
#else
#define M2CORE_EXPORT __declspec(dllimport)
#endif
#else
#define M2CORE_EXPORT __attribute__((visibility("default")))
#endif

/* Each core library defines only its own game's entry point. */

M2CORE_EXPORT const m2core_api *m2core_api_daytona93(void);
M2CORE_EXPORT const m2core_api *m2core_api_daytona(void);
M2CORE_EXPORT const m2core_api *m2core_api_srallyc(void);

#ifdef __cplusplus
}
#endif

#endif /* M2CORE_H */
