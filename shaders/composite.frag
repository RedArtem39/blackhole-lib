#version 450
#extension GL_GOOGLE_include_directive : require
// Bloom from the HDR mip chain, tonemap, lens touches, grain.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

#include "params.glsl"
layout(set = 0, binding = 1) uniform sampler2D hdr;

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

vec4 tap(float lod, vec2 uv) {
    vec2 ts = 1.0 / vec2(textureSize(hdr, int(lod)));
    vec4 s = textureLod(hdr, uv + vec2(-0.75, -0.75) * ts, lod)
           + textureLod(hdr, uv + vec2( 0.75, -0.75) * ts, lod)
           + textureLod(hdr, uv + vec2(-0.75,  0.75) * ts, lod)
           + textureLod(hdr, uv + vec2( 0.75,  0.75) * ts, lod);
    return s * 0.25;
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    vec2  uv     = vUV;
    vec2  c      = uv - 0.5;
    vec2  res    = P.outRes.xy;
    float aspect = res.x / res.y;
    bool  transparent = P.view.w > 0.5;

    // mild chromatic aberration toward the edges
    float ca = 0.006 * P.post2.y * dot(c, c);
    vec4 base = textureLod(hdr, uv, 0.0);
    vec3 col;
    col.r = textureLod(hdr, uv - c * ca, 0.0).r;
    col.g = base.g;
    col.b = textureLod(hdr, uv + c * ca, 0.0).b;
    float alpha = base.a;

    // bloom: radius shifts weight toward the wider mips
    int   levels = textureQueryLevels(hdr);
    float fall   = mix(0.45, 0.95, clamp(P.post.z, 0.0, 1.0));
    vec4  bloom  = vec4(0.0);
    float wsum   = 0.0;
    float w      = 1.0;
    for (int i = 1; i < 7; ++i) {
        if (i >= levels) break;
        bloom += tap(float(i), uv) * w;
        wsum  += w;
        w     *= fall;
    }
    if (wsum > 0.0) bloom /= wsum;

    float bs = P.post.y;
    col   = mix(col, bloom.rgb, clamp(0.22 * bs, 0.0, 1.0)) + bloom.rgb * 0.10 * bs;
    alpha = mix(alpha, bloom.a, clamp(0.22 * bs, 0.0, 1.0));
    col  *= P.post.x;

    // contrast around mid grey, before the tonemap
    float contrast = P.post2.w;
    col = 0.18 * pow(max(col / 0.18, 0.0), vec3(contrast));

    // tiny warm push in the highlights, cool shadows
    col = mix(col * vec3(0.96, 0.98, 1.04), col * vec3(1.03, 1.0, 0.96), smoothstep(0.0, 1.0, dot(col, vec3(0.333))));
    col = aces(col);

    float luma = dot(col, vec3(0.2126, 0.7152, 0.0722));
    col = max(mix(vec3(luma), col, P.post2.z), 0.0);

    float vig = smoothstep(1.15, 0.25, length(c * vec2(aspect, 1.0)));
    col *= mix(1.0 - 0.45 * P.post.w, 1.0, vig);

    if (transparent) {
        // premultiplied output: glow keeps its colour where there is no geometry
        alpha = clamp(max(alpha, max(col.r, max(col.g, col.b))), 0.0, 1.0);
    } else {
        alpha = 1.0;
    }

    // grain/dither in perceptual space
    vec3 g = pow(col, vec3(1.0 / 2.2));
    g += (hash12(uv * res + fract(P.resTime.z * 7.13) * 917.0) - 0.5) * (2.2 / 255.0) * P.post2.x * alpha;
    col = pow(max(g, 0.0), vec3(2.2));
    if (transparent) col = min(col, vec3(alpha));

    outColor = vec4(col, alpha);
}
