// Shared uniform block. Must match GpuParams in src/blackhole.cpp (std140, vec4 only).
layout(set = 0, binding = 0, std140) uniform Params {
    vec4 resTime;    // scene w, scene h, time, fov (rad)
    vec4 outRes;     // output w, output h, -, -
    vec4 cam;        // yaw, pitch, distance, roll
    vec4 view;       // center x, center y, lensing, transparent (0/1)
    vec4 diskGeo;    // inner radius, outer radius, thickness, rotation speed
    vec4 diskLook;   // brightness, opacity, detail, streaks
    vec4 diskPhys;   // doppler, redshift, temperature, flow period
    vec4 colEdge;
    vec4 colWarm;
    vec4 colHot;
    vec4 colCore;
    vec4 colShift;
    vec4 ring;       // rgb, intensity
    vec4 ringShape;  // width, radius, -, -
    vec4 sky;        // star density, star brightness, milky way, sky rotation
    vec4 skyTint;    // nebula tint rgb, seed
    vec4 bgColor;    // background rgb, -
    vec4 quality;    // max steps, step scale, -, -
    vec4 post;       // exposure, bloom strength, bloom radius, vignette
    vec4 post2;      // grain, chromatic aberration, saturation, contrast
} P;
