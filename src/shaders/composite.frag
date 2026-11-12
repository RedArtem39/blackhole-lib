#version 450
// Bloom from the HDR mip chain, tonemap, lens touches, grain.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdr;

layout(push_constant) uniform PC {
    vec2  res;
    float time;
    float exposure;
} pc;

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

vec3 tap(float lod, vec2 uv) {
    vec2 ts = 1.0 / vec2(textureSize(hdr, int(lod)));
    vec3 s = textureLod(hdr, uv + vec2(-0.75, -0.75) * ts, lod).rgb
           + textureLod(hdr, uv + vec2( 0.75, -0.75) * ts, lod).rgb
           + textureLod(hdr, uv + vec2(-0.75,  0.75) * ts, lod).rgb
           + textureLod(hdr, uv + vec2( 0.75,  0.75) * ts, lod).rgb;
    return s * 0.25;
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    vec2 uv = vUV;
    vec2 c  = uv - 0.5;
    float aspect = pc.res.x / pc.res.y;

    // mild chromatic aberration toward the edges
    float ca = 0.006 * dot(c, c);
    vec3 col;
    col.r = textureLod(hdr, uv - c * ca, 0.0).r;
    col.g = textureLod(hdr, uv, 0.0).g;
    col.b = textureLod(hdr, uv + c * ca, 0.0).b;

    int levels = textureQueryLevels(hdr);
    vec3 bloom = vec3(0.0);
    float wsum = 0.0;
    float w[7] = float[](0.0, 0.30, 0.26, 0.20, 0.13, 0.08, 0.05);
    for (int i = 1; i < 7; ++i) {
        if (i >= levels) break;
        bloom += tap(float(i), uv) * w[i];
        wsum  += w[i];
    }
    if (wsum > 0.0) bloom /= wsum;

    col = mix(col, bloom, 0.22) + bloom * 0.10;
    col *= pc.exposure;

    // tiny warm push in the highlights, cool shadows
    col = mix(col * vec3(0.96, 0.98, 1.04), col * vec3(1.03, 1.0, 0.96), smoothstep(0.0, 1.0, dot(col, vec3(0.333))));
    col = aces(col);

    float vig = smoothstep(1.15, 0.25, length(c * vec2(aspect, 1.0)));
    col *= mix(0.55, 1.0, vig);

    // grain/dither in perceptual space
    vec3 g = pow(col, vec3(1.0 / 2.2));
    g += (hash12(uv * pc.res + fract(pc.time * 7.13) * 917.0) - 0.5) * (2.2 / 255.0);
    col = pow(max(g, 0.0), vec3(2.2));

    outColor = vec4(col, 1.0);
}
