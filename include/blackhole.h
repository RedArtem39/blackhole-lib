/*
 * blackhole-lib — realtime raymarched black hole renderer (Vulkan).
 *
 * Plain C API. Everything visual is controlled through bh_params.
 *
 *   bh_renderer* r;
 *   bh_renderer_create(NULL, &r);
 *   bh_params p;
 *   bh_params_preset(&p, BH_PRESET_GARGANTUA);
 *   p.camera_pitch = 0.2f;
 *   p.ring_color = (bh_color){1.0f, 0.6f, 0.3f};
 *   bh_render_png(r, &p, 3840, 2160, "wallpaper.png");
 *   bh_renderer_destroy(r);
 *
 * Linking:
 *   static:  blackhole_static.lib + vulkan-1.lib (+ user32.lib)
 *   dynamic: blackhole.lib (import) + blackhole.dll, define BH_USE_DLL before including
 *
 * A renderer and everything created from it must be used from one thread at a time.
 */
#ifndef BLACKHOLE_H
#define BLACKHOLE_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(BH_BUILD_DLL)
#    define BH_API __declspec(dllexport)
#  elif defined(BH_USE_DLL)
#    define BH_API __declspec(dllimport)
#  else
#    define BH_API
#  endif
#else
#  define BH_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define BH_VERSION_MAJOR 1
#define BH_VERSION_MINOR 0
#define BH_VERSION_PATCH 0

typedef enum bh_result {
    BH_OK                     = 0,
    BH_SKIPPED                = 1,  /* nothing rendered (e.g. window minimized) — not an error */
    BH_ERROR_INVALID_ARGUMENT = -1,
    BH_ERROR_VULKAN           = -2,
    BH_ERROR_NO_DEVICE        = -3,
    BH_ERROR_IO               = -4,
    BH_ERROR_UNSUPPORTED      = -5,
    BH_ERROR_INTERNAL         = -6
} bh_result;

typedef enum bh_preset {
    BH_PRESET_GARGANTUA  = 0,  /* warm gold, the default look */
    BH_PRESET_FROST      = 1,  /* ice-blue disk, cold ring */
    BH_PRESET_CRIMSON    = 2,  /* deep red embers */
    BH_PRESET_MONOCHROME = 3,  /* silver, zero saturation */
    BH_PRESET_VOID       = 4,  /* faint disk, bright photon ring */
    BH_PRESET_QUASAR     = 5,  /* thick, very hot, heavy bloom */
    BH_PRESET_COUNT
} bh_preset;

typedef struct bh_color { float r, g, b; } bh_color;

/* All distances are in Schwarzschild radii (event horizon radius = 1). Angles in radians unless noted. */
typedef struct bh_params {
    uint32_t struct_size;          /* filled by bh_params_default(); leave as is */

    /* animation */
    float time;                    /* seconds; drives the gas flow. Advance it yourself. */

    /* camera (orbits the hole, always looks at it) */
    float camera_yaw;              /* around the hole                        default 0.55 */
    float camera_pitch;            /* above (+) / below (-) the disk plane   default 0.11, clamped to ±1.55 */
    float camera_distance;         /* from the centre                        default 24 */
    float camera_roll;             /* tilt of the frame                      default 0.07 */
    float fov_degrees;             /* vertical field of view                 default 45.8 */
    float center_x, center_y;      /* moves the hole on screen, in screen heights (+x right, +y up) */

    /* gravity */
    float lensing;                 /* light bending strength, 1 = normal, 0 = none */

    /* accretion disk */
    float disk_inner_radius;       /* default 2.3 */
    float disk_outer_radius;       /* default 15 */
    float disk_thickness;          /* multiplier, default 1 */
    float disk_brightness;         /* multiplier, default 1 */
    float disk_opacity;            /* multiplier, default 1 */
    float disk_rotation_speed;     /* multiplier, default 1; negative spins the other way */
    float disk_flow_period;        /* seconds per flow cycle, default 18 (longer = calmer) */
    float disk_detail;             /* 0..1 filament contrast, default 1 */
    float disk_streaks;            /* radial stretching of the gas, default 1 */
    float disk_temperature;        /* palette shift, >1 hotter, <1 cooler, default 1 */
    float doppler_strength;        /* 0..1 brightening of the approaching side, default 1 */
    float redshift_strength;       /* 0..1 dimming near the horizon, default 1 */
    bh_color disk_color_edge;      /* coldest outer gas */
    bh_color disk_color_warm;
    bh_color disk_color_hot;
    bh_color disk_color_core;      /* inner edge */
    bh_color disk_color_shift;     /* blue-shifted highlight on the approaching side */

    /* photon ring (thin glow hugging the shadow) */
    bh_color ring_color;
    float ring_intensity;          /* 0 disables, default 0.35 */
    float ring_width;              /* default 0.06 */
    float ring_radius;             /* default 1.5 (photon sphere) */

    /* background */
    int   transparent_background;  /* 1: no sky, output alpha (premultiplied) */
    bh_color background_color;     /* flat colour under the stars, default black */
    float star_density;            /* multiplier, default 1 */
    float star_brightness;         /* multiplier, default 1 */
    float milky_way;               /* brightness of the galactic band, default 1 */
    bh_color nebula_tint;          /* tint of the galactic band, default white */
    float sky_rotation;            /* rotates the star field, radians */
    uint32_t seed;                 /* different star field / gas pattern */

    /* post processing */
    float exposure;                /* default 1 */
    float bloom_strength;          /* default 1 */
    float bloom_radius;            /* 0..1, default 0.6 */
    float vignette;                /* 0..1, default 1 */
    float grain;                   /* default 1 */
    float chromatic_aberration;    /* default 1 */
    float saturation;              /* default 1 */
    float contrast;                /* default 1 */

    /* quality */
    float render_scale;            /* internal resolution, 0.25..4; >1 supersamples (wallpapers) */
    int   max_steps;               /* ray steps per pixel, default 560 */
    float step_scale;              /* <1 = finer steps, slower; default 1 */
} bh_params;

typedef struct bh_renderer_desc {
    uint32_t struct_size;          /* filled by bh_renderer_desc_default() */
    int enable_validation;         /* Vulkan validation layer, messages go to stderr */
    int gpu_index;                 /* -1 = pick the best GPU */
} bh_renderer_desc;

typedef struct bh_renderer bh_renderer;
typedef struct bh_target   bh_target;

/* ---- params ---- */
BH_API void        bh_params_default(bh_params* p);
BH_API bh_result   bh_params_preset(bh_params* p, bh_preset preset);   /* resets p, then applies the preset */
BH_API const char* bh_preset_name(bh_preset preset);
BH_API int         bh_preset_from_name(const char* name);              /* -1 if unknown, case-insensitive */
/* out = a + (b - a) * t for every numeric field (ints/seed snap at t = 0.5). out may alias a or b. */
BH_API void        bh_params_lerp(bh_params* out, const bh_params* a, const bh_params* b, float t);

/* ---- errors ---- */
BH_API const char* bh_get_last_error(void);    /* message of the last failure on this thread */
BH_API const char* bh_result_string(bh_result r);

/* ---- renderer ---- */
BH_API void        bh_renderer_desc_default(bh_renderer_desc* d);
BH_API bh_result   bh_renderer_create(const bh_renderer_desc* desc /* may be NULL */, bh_renderer** out);
BH_API void        bh_renderer_destroy(bh_renderer* r);
BH_API const char* bh_renderer_gpu_name(const bh_renderer* r);

/* ---- offscreen ---- */
/* Writes width*height RGBA8 pixels (sRGB, top row first). row_stride 0 = width*4.
 * With transparent_background the alpha is premultiplied. */
BH_API bh_result bh_render_rgba8(bh_renderer* r, const bh_params* p, uint32_t width, uint32_t height,
                                 void* pixels, size_t row_stride);
/* Renders and writes a PNG (UTF-8 path). Transparent renders are saved with straight alpha. */
BH_API bh_result bh_render_png(bh_renderer* r, const bh_params* p, uint32_t width, uint32_t height,
                               const char* path_utf8);
/* Plain PNG writer for RGBA8 data. premultiplied != 0 converts to straight alpha first. */
BH_API bh_result bh_write_png(const char* path_utf8, const void* rgba, uint32_t width, uint32_t height,
                              size_t row_stride, int premultiplied);

/* ---- window target (Windows) ---- */
/* Renders straight into a window (top-level or child). Size follows the client area automatically. */
BH_API bh_result bh_target_create_win32(bh_renderer* r, void* hwnd, bh_target** out);
BH_API bh_result bh_target_present(bh_target* t, const bh_params* p);   /* BH_SKIPPED when the window has no area */
BH_API void      bh_target_set_vsync(bh_target* t, int enabled);        /* default on */
BH_API void      bh_target_destroy(bh_target* t);

#ifdef __cplusplus
}
#endif

#endif /* BLACKHOLE_H */
