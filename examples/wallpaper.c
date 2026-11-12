/*
 * blackhole-lib example (plain C): render a wallpaper PNG, optionally set it as the desktop background.
 *
 *   wallpaper.exe --preset frost --pitch 0.25 --yaw 1.2 --size 3840x2160 --out frost.png
 *   wallpaper.exe --preset crimson --ring 1,0.3,0.2 --ring-intensity 1.5 --set
 *
 * Options:
 *   --preset NAME        gargantua | frost | crimson | monochrome | void | quasar
 *   --size WxH           output size (default: primary monitor)
 *   --out PATH           output file (default blackhole_wallpaper.png)
 *   --yaw R --pitch R --roll R --dist D --fov DEG     camera
 *   --center X,Y         move the hole on screen (screen heights)
 *   --time T             animation time (changes the gas pattern)
 *   --seed N             different star field
 *   --ring R,G,B  --ring-intensity I                  photon ring
 *   --disk-hot R,G,B --disk-warm R,G,B                recolour parts of the disk
 *   --exposure E  --scale S (supersampling, default 1.5)
 *   --transparent        no background, alpha PNG
 *   --set                set as desktop wallpaper (Windows)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blackhole.h"

static int parseColor(const char* s, bh_color* c) {
    return sscanf(s, "%f,%f,%f", &c->r, &c->g, &c->b) == 3;
}

int main(int argc, char** argv) {
    bh_params p;
    SetProcessDPIAware();   /* real monitor pixels, not scaled ones */
    bh_params_default(&p);
    p.render_scale = 1.5f;

    uint32_t w = (uint32_t)GetSystemMetrics(SM_CXSCREEN), h = (uint32_t)GetSystemMetrics(SM_CYSCREEN);
    const char* out = "blackhole_wallpaper.png";
    int setWallpaper = 0;

    /* preset first so that later flags override it */
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--preset")) {
            int id = bh_preset_from_name(argv[i + 1]);
            if (id < 0) {
                fprintf(stderr, "unknown preset '%s'\n", argv[i + 1]);
                return 1;
            }
            bh_params_preset(&p, (bh_preset)id);
            p.render_scale = 1.5f;
        }
    }

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : NULL;
#define NEXT() (i++, v)
        if (!strcmp(a, "--preset") && v) NEXT();
        else if (!strcmp(a, "--size") && v) { if (sscanf(NEXT(), "%ux%u", &w, &h) != 2) { fprintf(stderr, "bad --size\n"); return 1; } }
        else if (!strcmp(a, "--out") && v) out = NEXT();
        else if (!strcmp(a, "--yaw") && v) p.camera_yaw = (float)atof(NEXT());
        else if (!strcmp(a, "--pitch") && v) p.camera_pitch = (float)atof(NEXT());
        else if (!strcmp(a, "--roll") && v) p.camera_roll = (float)atof(NEXT());
        else if (!strcmp(a, "--dist") && v) p.camera_distance = (float)atof(NEXT());
        else if (!strcmp(a, "--fov") && v) p.fov_degrees = (float)atof(NEXT());
        else if (!strcmp(a, "--center") && v) sscanf(NEXT(), "%f,%f", &p.center_x, &p.center_y);
        else if (!strcmp(a, "--time") && v) p.time = (float)atof(NEXT());
        else if (!strcmp(a, "--seed") && v) p.seed = (uint32_t)strtoul(NEXT(), NULL, 10);
        else if (!strcmp(a, "--ring") && v) parseColor(NEXT(), &p.ring_color);
        else if (!strcmp(a, "--ring-intensity") && v) p.ring_intensity = (float)atof(NEXT());
        else if (!strcmp(a, "--disk-hot") && v) parseColor(NEXT(), &p.disk_color_hot);
        else if (!strcmp(a, "--disk-warm") && v) parseColor(NEXT(), &p.disk_color_warm);
        else if (!strcmp(a, "--exposure") && v) p.exposure = (float)atof(NEXT());
        else if (!strcmp(a, "--scale") && v) p.render_scale = (float)atof(NEXT());
        else if (!strcmp(a, "--transparent")) p.transparent_background = 1;
        else if (!strcmp(a, "--set")) setWallpaper = 1;
        else {
            fprintf(stderr, "unknown or incomplete option '%s'\n", a);
            return 1;
        }
#undef NEXT
    }

    bh_renderer* r = NULL;
    if (bh_renderer_create(NULL, &r) != BH_OK) {
        fprintf(stderr, "renderer: %s\n", bh_get_last_error());
        return 1;
    }
    printf("GPU: %s\nrendering %ux%u (x%.2f supersampling)...\n", bh_renderer_gpu_name(r), w, h, p.render_scale);

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    bh_result res = bh_render_png(r, &p, w, h, out);
    QueryPerformanceCounter(&t1);
    bh_renderer_destroy(r);
    if (res != BH_OK) {
        fprintf(stderr, "render failed: %s (%s)\n", bh_get_last_error(), bh_result_string(res));
        return 1;
    }
    printf("saved %s in %.0f ms\n", out, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart);

    if (setWallpaper) {
        wchar_t wout[MAX_PATH], full[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, out, -1, wout, MAX_PATH);
        GetFullPathNameW(wout, MAX_PATH, full, NULL);
        if (!SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, full, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) {
            fprintf(stderr, "could not set wallpaper\n");
            return 1;
        }
        printf("wallpaper set\n");
    }
    return 0;
}
