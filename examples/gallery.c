/*
 * blackhole-lib example (plain C, links the DLL): render every preset from a few angles.
 *
 *   gallery.exe [out_dir] [width] [height]
 */
#include <stdio.h>
#include <stdlib.h>

#include "blackhole.h"

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "gallery";
    uint32_t w = argc > 2 ? (uint32_t)atoi(argv[2]) : 1280;
    uint32_t h = argc > 3 ? (uint32_t)atoi(argv[3]) : 720;

    bh_renderer_desc d;
    bh_renderer_desc_default(&d);
    d.enable_validation = getenv("BH_VALIDATE") != NULL;
    bh_renderer* r = NULL;
    if (bh_renderer_create(&d, &r) != BH_OK) {
        fprintf(stderr, "renderer: %s\n", bh_get_last_error());
        return 1;
    }
    printf("GPU: %s\n", bh_renderer_gpu_name(r));

    struct { const char* name; float yaw, pitch, dist, time; } views[] = {
        {"side", 0.55f, 0.11f, 24.0f, 20.0f},
        {"high", 2.10f, 0.55f, 26.0f, 50.0f},
        {"close", 4.00f, 0.05f, 13.0f, 90.0f},
    };

    char path[512];
    int failures = 0;
    for (int pr = 0; pr < BH_PRESET_COUNT; pr++) {
        for (int v = 0; v < 3; v++) {
            bh_params p;
            bh_params_preset(&p, (bh_preset)pr);
            p.camera_yaw = views[v].yaw;
            p.camera_pitch = views[v].pitch;
            p.camera_distance = views[v].dist;
            p.time = views[v].time;
            snprintf(path, sizeof path, "%s/%s_%s.png", dir, bh_preset_name((bh_preset)pr), views[v].name);
            if (bh_render_png(r, &p, w, h, path) != BH_OK) {
                fprintf(stderr, "%s: %s\n", path, bh_get_last_error());
                failures++;
            } else {
                printf("%s\n", path);
            }
        }
    }

    /* API features: custom colours, off-centre framing, transparency, lerp */
    bh_params a, b, m;
    bh_params_preset(&a, BH_PRESET_GARGANTUA);
    bh_params_preset(&b, BH_PRESET_FROST);
    bh_params_lerp(&m, &a, &b, 0.5f);
    m.time = 20.0f;
    snprintf(path, sizeof path, "%s/lerp_gargantua_frost.png", dir);
    failures += bh_render_png(r, &m, w, h, path) != BH_OK;

    bh_params c;
    bh_params_default(&c);
    c.time = 20.0f;
    c.ring_color = (bh_color){0.4f, 1.0f, 0.7f};
    c.ring_intensity = 2.0f;
    c.center_x = 0.35f;
    c.camera_roll = -0.2f;
    snprintf(path, sizeof path, "%s/custom_ring_offcenter.png", dir);
    failures += bh_render_png(r, &c, w, h, path) != BH_OK;

    bh_params t;
    bh_params_default(&t);
    t.time = 20.0f;
    t.transparent_background = 1;
    t.vignette = 0.0f;
    snprintf(path, sizeof path, "%s/transparent.png", dir);
    failures += bh_render_png(r, &t, w, h, path) != BH_OK;

    /* error path: params not initialised */
    bh_params bad = {0};
    unsigned char px[16];
    if (bh_render_rgba8(r, &bad, 2, 2, px, 0) != BH_ERROR_INVALID_ARGUMENT) {
        fprintf(stderr, "expected invalid-argument error\n");
        failures++;
    } else {
        printf("error path ok: %s\n", bh_get_last_error());
    }

    bh_renderer_destroy(r);
    printf("%s\n", failures ? "FAILED" : "all done");
    return failures ? 1 : 0;
}
