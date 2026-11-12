#version 450
#extension GL_GOOGLE_include_directive : require
// Black hole: bent-ray marching through a volumetric accretion disk.
// Units: Schwarzschild radius = 1. Looks first, physics second.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

#include "params.glsl"

const float PI = 3.14159265359;

vec3 g_seedOff;

// ---------------------------------------------------------------- hashing / noise
uvec3 pcg3d(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
vec3 hash33(vec3 p) { return vec3(pcg3d(uvec3(ivec3(floor(p))))) * (1.0 / 4294967295.0); }

float hashI(ivec3 c) {
    uvec3 q = uvec3(c) * uvec3(1597334673u, 3812015801u, 2798796415u);
    uint n = (q.x ^ q.y ^ q.z) * 1597334673u;
    return float(n) * (1.0 / 4294967296.0);
}

float vnoise(vec3 p) {
    ivec3 i = ivec3(floor(p));
    vec3  f = fract(p);
    vec3  u = f * f * (3.0 - 2.0 * f);
    float a = hashI(i),                  b = hashI(i + ivec3(1, 0, 0));
    float c = hashI(i + ivec3(0, 1, 0)), d = hashI(i + ivec3(1, 1, 0));
    float e = hashI(i + ivec3(0, 0, 1)), g = hashI(i + ivec3(1, 0, 1));
    float h = hashI(i + ivec3(0, 1, 1)), k = hashI(i + ivec3(1, 1, 1));
    return mix(mix(mix(a, b, u.x), mix(c, d, u.x), u.y),
               mix(mix(e, g, u.x), mix(h, k, u.x), u.y), u.z);
}

const mat3 ROT = mat3( 0.00,  0.80,  0.60,
                      -0.80,  0.36, -0.48,
                      -0.60, -0.48,  0.64);

float fbm(vec3 p, int oct) {
    float s = 0.0, a = 0.5, n = 0.0;
    for (int i = 0; i < oct; ++i) {
        s += a * vnoise(p);
        n += a;
        p = ROT * p * 2.03 + vec3(3.1, 1.7, 5.3);
        a *= 0.5;
    }
    return s / n;
}

// ---------------------------------------------------------------- disk
vec3 heat(float t) {
    t = clamp(t, 0.0, 1.7);
    vec3 c = mix(P.colEdge.rgb, P.colWarm.rgb, smoothstep(0.00, 0.45, t));
    c = mix(c, P.colHot.rgb,   smoothstep(0.35, 0.80, t));
    c = mix(c, P.colCore.rgb,  smoothstep(0.70, 1.15, t));
    c = mix(c, P.colShift.rgb, smoothstep(1.10, 1.70, t));
    return c;
}

float diskHalfThickness(float rc) { return (0.028 + 0.011 * rc) * P.diskGeo.z; }

// Anisotropic noise: long along the orbit, tight across radius.
float streaks(float ang, float lr, float y, float seed) {
    float s = P.diskLook.w;
    vec3 q = vec3(cos(ang) * 2.2, sin(ang) * 2.2, lr * 13.0 * s) + vec3(seed, -seed * 0.7, y * 0.6) + g_seedOff;
    float n = fbm(q, 4);
    vec3 q2 = vec3(cos(ang) * 9.0, sin(ang) * 9.0, lr * 70.0 * s) + vec3(-seed, seed, y) - g_seedOff;
    float m = fbm(q2, 3);
    return n * 0.62 + m * 0.48;
}

float diskSample(vec3 p, vec3 rayDir, float r, out vec3 emis) {
    emis = vec3(0.0);
    float rIn = P.diskGeo.x, rOut = P.diskGeo.y;
    float rc = length(p.xz);
    float H  = diskHalfThickness(rc);
    float y  = p.y / H;
    float vert   = exp(-y * y);
    float radial = smoothstep(rIn - 0.2, rIn + 0.9, rc) * (1.0 - smoothstep(mix(rIn, rOut, 0.28), rOut, rc));
    float env = vert * radial;
    if (env < 2e-3) return 0.0;

    float phi    = atan(p.z, p.x);
    float omega  = 1.7 * P.diskGeo.w * pow(rc, -1.5);
    float lr     = log(rc);
    float period = P.diskPhys.w;

    float t0 = fract(P.resTime.z / period);
    float t1 = fract(P.resTime.z / period + 0.5);
    float w0 = 1.0 - abs(2.0 * t0 - 1.0);
    float n0 = streaks(phi - omega * t0 * period, lr, y, 0.0);
    float n1 = streaks(phi - omega * t1 * period, lr, y, 11.7);
    float n  = mix(n1, n0, w0);
    // restore the contrast lost by blending two independent fields
    n = (n - 0.5) / sqrt(w0 * w0 + (1.0 - w0) * (1.0 - w0)) + 0.5;

    // a few bright rings / gaps across radius
    float rings = 0.65 + 0.35 * sin(lr * 23.0 + 1.3 * sin(lr * 7.0));

    float fil  = smoothstep(0.62 - 0.32 * P.diskLook.z, 0.95, n);
    float dens = env * (0.015 + 2.6 * fil * fil * rings);

    // temperature, doppler beaming, gravitational redshift
    float tr    = pow(rIn / rc, 0.85);
    vec3  vdir  = vec3(-sin(phi), 0.0, cos(phi)) * sign(P.diskGeo.w + 1e-6);
    float beta  = min(sqrt(0.5 / rc), 0.6) * 0.85;
    float gamma = inversesqrt(1.0 - beta * beta);
    float g     = 1.0 / (gamma * (1.0 - beta * dot(vdir, -rayDir)));
    g = mix(1.0, g, P.diskPhys.x);
    float grav  = mix(1.0, sqrt(max(1.0 - 1.0 / r, 0.0)), P.diskPhys.y);
    float gt    = g * grav;

    vec3  col = heat(tr * mix(1.0, gt, 0.65) * 1.12 * P.diskPhys.z);
    float I   = (0.06 + 7.5 * pow(tr, 2.4)) * pow(gt, 1.9) * P.diskLook.x;
    emis = col * I * dens;
    return dens;
}

// ---------------------------------------------------------------- sky
vec3 starLayer(vec3 d, float scale, float density, float pa) {
    vec3 p = d * scale;
    vec3 base = floor(p);
    vec3 col = vec3(0.0);
    vec3 seedCell = floor(g_seedOff * 7.0);
    float sigma = max(pa * 0.62, 1e-6) * scale;   // in cell units
    for (int z = -1; z <= 1; ++z)
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
        vec3 cell = base + vec3(x, y, z);
        vec3 h2 = hash33(cell + seedCell + vec3(17.0, 31.0, 7.0));
        if (h2.x > density) continue;
        vec3 sp = cell + 0.15 + 0.7 * hash33(cell + seedCell);
        vec3 dv = p - sp;
        float radialOff = dot(dv, d);
        dv -= d * radialOff;
        float b = pow(h2.y, 22.0) * 70.0 + 0.035 * h2.y;
        b *= exp(-radialOff * radialOff * 6.0);       // stars far off the shell fade out
        float core = exp(-dot(dv, dv) / (2.0 * sigma * sigma));
        vec3 tint = mix(vec3(1.0, 0.72, 0.50), vec3(0.68, 0.80, 1.0), h2.z);
        col += mix(tint, vec3(1.0), 0.45) * b * core;
    }
    return col;
}

vec3 sky(vec3 d, float pa) {
    float cr = cos(P.sky.w), sr = sin(P.sky.w);
    d = vec3(cr * d.x - sr * d.z, d.y, sr * d.x + cr * d.z);

    vec3  n    = normalize(vec3(0.28, 1.0, -0.42));       // galactic pole, tilted against the disk
    float lat  = dot(d, n);
    float band = exp(-lat * lat * 10.0);
    vec3  so   = g_seedOff * 0.1;
    float neb  = fbm(d * 2.6 + 3.0 + so, 5);
    float fine = fbm(d * 9.0 - 1.0 + so, 4);
    float dust = smoothstep(0.48, 0.72, fbm(d * 5.5 + vec3(9.0, 2.0, 4.0) + so, 5));
    vec3  mw = mix(vec3(0.28, 0.31, 0.44), vec3(0.62, 0.46, 0.33), smoothstep(0.35, 0.7, fine));
    mw *= band * smoothstep(0.25, 0.85, neb) * (1.0 - 0.9 * dust * band);
    vec3 col = mw * 0.07 * P.sky.z * P.skyTint.rgb;
    col += vec3(0.035, 0.045, 0.08) * 0.03 * fbm(d * 1.4 + 7.0 + so, 4) * P.sky.z * P.skyTint.rgb;
    vec3 stars = starLayer(d, 60.0, (0.035 + 0.10 * band) * P.sky.x, pa)
               + starLayer(d, 150.0, (0.015 + 0.10 * band) * P.sky.x, pa) * 0.6;
    col += stars * P.sky.y;
    return col + P.bgColor.rgb;
}

// ---------------------------------------------------------------- trace
vec4 render(vec3 ro, vec3 rd, float jitter, float pa) {
    vec3  pos = ro;
    vec3  vel = rd;
    vec3  hv  = cross(pos, vel);
    float h2  = dot(hv, hv) * P.view.z;

    vec3  acc   = vec3(0.0);
    float trans = 1.0;
    bool  escaped  = false;
    bool  jittered = false;

    int   maxSteps  = int(P.quality.x);
    float stepScale = P.quality.y;
    float rOut      = P.diskGeo.y;
    float ringR     = P.ringShape.y;
    float ringW     = P.ringShape.x;
    bool  ringOn    = P.ring.w > 0.0;

    for (int i = 0; i < 4096; ++i) {
        if (i >= maxSteps) break;
        float r2 = dot(pos, pos);
        float r  = sqrt(r2);
        if (r < 1.0) break;
        if (r > max(60.0, P.cam.z * 2.5) && dot(pos, vel) > 0.0) { escaped = true; break; }

        float dt = clamp(0.11 * r - 0.06, 0.025, 2.5) * stepScale;
        float rc = length(pos.xz);
        if (rc < rOut + 2.0) {
            float H    = diskHalfThickness(rc);
            float slab = abs(pos.y) - 3.0 * H;
            dt = min(dt, max(slab * 0.8, 0.32 * H * stepScale));
            if (!jittered && slab < 0.0) { dt *= jitter; jittered = true; }
        }

        vel += -1.5 * h2 * pos / (r2 * r2 * r) * dt;
        pos += vel * dt;

        float stepLen = dt * length(vel);
        float rn = length(pos);

        if (ringOn) {
            float x  = (rn - ringR) / ringW;
            float tang = 1.0 - abs(dot(pos, vel)) / (rn * length(vel));
            acc += trans * P.ring.rgb * P.ring.w * exp(-x * x) * pow(tang, 6.0) * stepLen;
        }

        vec3  e;
        float d = diskSample(pos, normalize(vel), rn, e);
        if (d > 0.0) {
            acc   += trans * e * stepLen;
            trans *= exp(-d * 1.15 * P.diskLook.y * stepLen);
            if (trans < 0.004) break;
        }
    }

    float alpha;
    if (P.view.w > 0.5) {
        alpha = escaped ? 1.0 - trans : 1.0;
    } else {
        if (escaped) acc += trans * sky(normalize(vel), pa);
        alpha = 1.0;
    }
    return vec4(acc, alpha);
}

void main() {
    vec2 res  = P.resTime.xy;
    vec2 frag = vec2(vUV.x, 1.0 - vUV.y) * res;
    vec2 p = (frag - 0.5 * res) / res.y - P.view.xy;

    g_seedOff = vec3(P.skyTint.w * 17.13, P.skyTint.w * 5.71, P.skyTint.w * 11.37);

    float fov = P.resTime.w;
    float yaw = P.cam.x, pitch = P.cam.y, dist = P.cam.z, roll = P.cam.w;
    vec3 ro = dist * vec3(cos(pitch) * cos(yaw), sin(pitch), cos(pitch) * sin(yaw));
    vec3 fw = normalize(-ro);
    vec3 rt = normalize(cross(fw, vec3(0.0, 1.0, 0.0)));
    vec3 up = cross(rt, fw);
    float cr = cos(roll), sr = sin(roll);
    vec3 rt2 = rt * cr + up * sr;
    vec3 up2 = up * cr - rt * sr;
    float f  = 0.5 / tan(fov * 0.5);
    vec3 rd  = normalize(fw * f + rt2 * p.x + up2 * p.y);

    float pa = 2.0 * tan(fov * 0.5) / res.y;
    float jitter = fract(52.9829189 * fract(dot(frag, vec2(0.06711056, 0.00583715))));

    vec4 col = render(ro, rd, jitter, pa);
    if (any(isnan(col)) || any(isinf(col))) col = vec4(0.0, 0.0, 0.0, 1.0);
    outColor = vec4(min(col.rgb, vec3(60000.0)), clamp(col.a, 0.0, 1.0));
}
