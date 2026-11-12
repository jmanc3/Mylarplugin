#pragma once
// Embedded so installed builds need no runtime shader files.
namespace drawing::gl_shaders {
inline constexpr const char *vertex = R"GLSL(#version 300 es
precision highp float;
layout(location=0) in vec2 position;
layout(location=1) in vec2 texcoord;
layout(location=2) in vec4 color;
layout(location=3) in float opacity;
uniform vec4 target; // device-space origin and size
out vec2 uv;
out vec2 device;
out vec4 tint;
out float maskOpacity;
void main() {
    vec2 p = (position - target.xy) / target.zw;
    gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    uv = texcoord;
    device = position;
    tint = color;
    maskOpacity = opacity;
}
)GLSL";
inline constexpr const char *fragment = R"GLSL(#version 300 es
precision highp float;
in vec2 uv;
in vec2 device;
in vec4 tint;
in float maskOpacity;
uniform sampler2D sourceTexture;
uniform sampler2D clipTexture;
// 0 solid, 1 premultiplied RGBA, 2 glyph coverage, 3 bounded texture source,
// 6 linear gradient
uniform int sourceKind;
uniform int hasClip;
uniform vec4 clipRect;
uniform vec4 clipTarget;
uniform int erasePass;
uniform vec4 gradientLine; // device-space start and end points
uniform vec4 gradientFrom; // premultiplied
uniform vec4 gradientTo;   // premultiplied
out vec4 result;
void main() {
    vec2 edge = min(device - clipRect.xy, clipRect.xy + clipRect.zw - device);
    float coverage = clamp(edge.x + 0.5, 0.0, 1.0) * clamp(edge.y + 0.5, 0.0, 1.0);
    if (hasClip != 0) {
        vec2 p = (device - clipTarget.xy) / clipTarget.zw;
        coverage *= texture(clipTexture, vec2(p.x, 1.0 - p.y)).a;
    }
    vec4 value = tint;
    if (sourceKind == 1 || sourceKind == 3) {
        vec4 sampleColor = texture(sourceTexture, uv);
        if (sourceKind == 3 && (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))))
            sampleColor = vec4(0.0);
        value *= sampleColor;
    }
    if (sourceKind == 6) {
        vec2 d = gradientLine.zw - gradientLine.xy;
        float len = max(dot(d, d), 1e-6);
        float t = clamp(dot(device - gradientLine.xy, d) / len, 0.0, 1.0);
        value *= mix(gradientFrom, gradientTo, t);
    }
    if (sourceKind == 2) coverage *= texture(sourceTexture, uv).r;
    result = erasePass != 0 ? vec4(0.0, 0.0, 0.0, coverage * maskOpacity)
                            : value * coverage;
}
)GLSL";
} // namespace drawing::gl_shaders
