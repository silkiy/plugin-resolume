#pragma once

inline constexpr char beatVertexShader[] = R"(#version 410 core
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 texCoord;
out vec2 uv;

void main()
{
    gl_Position = vec4(position, 1.0);
    uv = texCoord;
}
)";

inline constexpr char beatFragmentShader[] = R"(#version 410 core
in vec2 uv;
out vec4 fragColor;

uniform sampler2D inputTexture;
uniform vec2 inputMaxUV;
uniform vec2 inputHalfTexel;
uniform int pattern;
uniform int gridSize;
uniform int stepIndex;
uniform float amount;
uniform float pulse;
uniform float frameAspect;

vec4 sampleInput(vec2 point)
{
    // Clip to the centers of the visible texels, excluding any backing texture padding.
    return texture(inputTexture, clamp(point * inputMaxUV, inputHalfTexel, inputMaxUV - inputHalfTexel));
}

int shuffleIndex(int index, int count)
{
    // An odd stride is coprime to the power-of-two tile count, so every tile is sampled once.
    uint hash = (uint(stepIndex) + 1u) * 0x9E3779B9u;
    int stride = int((hash >> 16u) & uint(count - 1)) | 1;
    int offset = 1 + stepIndex % (count - 1);
    return (index * stride + offset) % count;
}

vec2 rotatePoint(vec2 point, float angle)
{
    float c = cos(angle);
    float s = sin(angle);
    return vec2(c * point.x - s * point.y, s * point.x + c * point.y);
}

void main()
{
    vec4 original = sampleInput(uv);
    float strength = clamp(amount * pulse, 0.0, 1.0);
    if (strength <= 0.0) {
        fragColor = original;
        return;
    }

    if (pattern == 8) {
        // Quantized centers make the burst readable even on low-resolution inputs.
        vec2 cells = vec2(float(gridSize * 2)) * vec2(max(frameAspect, 0.001), 1.0);
        vec2 blockUV = (floor(uv * cells) + 0.5) / cells;
        vec4 blocks = sampleInput(blockUV);
        fragColor = mix(original, blocks, strength);
        return;
    }

    if (pattern == 9) {
        vec2 aspect = vec2(max(frameAspect, 0.001), 1.0);
        vec2 point = (uv - 0.5) * aspect;
        float direction = stepIndex % 2 == 0 ? 1.0 : -1.0;
        float angle = direction * 0.85 * strength;
        vec2 center = rotatePoint(point, angle) / (1.0 + 0.35 * strength);
        vec4 base = sampleInput(0.5 + center / aspect);
        vec4 red = sampleInput(0.5 + rotatePoint(center, 0.13 * strength) / aspect);
        vec4 blue = sampleInput(0.5 + rotatePoint(center, -0.13 * strength) / aspect);
        vec3 prism = vec3(red.a > 0.0 ? red.r / red.a * base.a : 0.0, base.g,
            blue.a > 0.0 ? blue.b / blue.a * base.a : 0.0);
        fragColor = mix(original, vec4(prism, base.a), strength);
        return;
    }

    if (pattern == 10) {
        vec2 repeatUV = uv * float(gridSize / 2);
        vec2 cell = floor(repeatUV);
        vec2 folded = fract(repeatUV);
        if (mod(cell.x + float(stepIndex), 2.0) < 1.0) folded.x = 1.0 - folded.x;
        if (mod(cell.y + float(stepIndex / 2), 2.0) < 1.0) folded.y = 1.0 - folded.y;
        fragColor = mix(original, sampleInput(folded), strength);
        return;
    }

    if (pattern == 11) {
        vec2 point = uv - 0.5;
        float edgeRadius = max(abs(point.x), abs(point.y));
        float ring = floor(edgeRadius * float(gridSize * 2));
        float scale = 1.0 + ring * 0.45 + 0.6 * strength;
        float turn = (stepIndex % 2 == 0 ? 1.0 : -1.0) * ring * 0.14 * strength;
        vec2 nested = rotatePoint(point * scale, turn);
        vec4 echo = sampleInput(fract(nested + 0.5));
        float boundary = smoothstep(0.0, 0.08, fract(edgeRadius * float(gridSize * 2)));
        echo.rgb *= mix(0.35, 1.0, boundary);
        fragColor = mix(original, echo, strength);
        return;
    }

    if (pattern == 12) {
        // One sweep travels across the image during a pulse.
        bool horizontal = stepIndex % 2 == 0;
        float axis = horizontal ? uv.y : uv.x;
        float center = 0.15 + 0.7 * (1.0 - pulse);
        float band = 1.0 - smoothstep(0.04, 0.18, abs(axis - center));
        vec2 offset = horizontal ? vec2(0.15 * strength * band, 0.0)
            : vec2(0.0, 0.15 * strength * band);
        vec4 sampled = sampleInput(fract(uv + offset));
        vec3 tint = sampled.a > 0.0 ? sampled.rgb / sampled.a * original.a : vec3(0.0);
        tint = mix(tint, original.a * vec3(0.12, 0.85, 1.0), 0.4);
        fragColor = vec4(mix(original.rgb, tint, strength * band), original.a);
        return;
    }

    if (pattern == 5) {
        float row = floor(uv.y * float(gridSize * 2));
        float jitter = sin(row * 17.0 + float(stepIndex) * 3.7);
        vec2 shift = vec2((0.045 + 0.065 * jitter) * strength, 0.0);
        vec4 red = sampleInput(fract(uv + shift));
        vec4 blue = sampleInput(fract(uv - shift));
        vec3 split = vec3(red.a > 0.0 ? red.r / red.a * original.a : 0.0,
            original.g, blue.a > 0.0 ? blue.b / blue.a * original.a : 0.0);
        fragColor = vec4(mix(original.rgb, split, strength), original.a);
        return;
    }

    if (pattern == 6) {
        vec2 direction = uv - 0.5;
        float zoom = 1.0 + 1.4 * strength;
        vec4 close = sampleInput(0.5 + direction / zoom);
        vec4 echo = sampleInput(0.5 + direction / (zoom + 0.18 * strength));
        vec4 farEcho = sampleInput(0.5 + direction / (zoom + 0.36 * strength));
        fragColor = mix(original, close * 0.6 + echo * 0.25 + farEcho * 0.15, strength);
        return;
    }

    if (pattern == 7) {
        const float fullTurn = 6.283185307;
        vec2 aspect = vec2(max(frameAspect, 0.001), 1.0);
        vec2 point = (uv - 0.5) * aspect;
        float radius = length(point);
        float angle = radius > 0.00001 ? atan(point.y, point.x) : 0.0;
        float sector = fullTurn / float(gridSize * 2);
        angle += float(stepIndex % 8) * 0.2;
        float folded = abs(mod(angle, sector) - sector * 0.5);
        vec2 mirrored = radius * vec2(cos(folded), sin(folded));
        vec4 kaleido = sampleInput(0.5 + mirrored / aspect / (1.0 + 0.4 * strength));
        fragColor = mix(original, kaleido, strength);
        return;
    }

    if (pattern == 3) {
        // One spatial shutter per hit. Time only controls the fade, never extra flashes.
        vec2 shutterUV = (stepIndex % 2 == 0) ? uv : uv.yx;
        float band = floor(shutterUV.x * 8.0);
        float shutter = mod(band + float(stepIndex), 2.0);
        vec3 bright = mix(original.rgb, vec3(original.a), 0.95 * strength);
        vec3 dark = original.rgb * (1.0 - 0.9 * strength);
        fragColor = vec4(mix(dark, bright, shutter), original.a);
        return;
    }

    float grid = float(gridSize);
    vec2 gridUV = uv * grid;
    ivec2 tile = min(ivec2(gridUV), ivec2(gridSize - 1));
    vec2 local = gridUV - vec2(tile);

    if (pattern == 0) {
        float direction = ((tile.y + stepIndex % 2) % 2 == 0) ? 1.0 : -1.0;
        fragColor = sampleInput(vec2(fract(uv.x + direction * 0.25 * strength), uv.y));
    } else if (pattern == 1) {
        const float fullTurn = 6.283185307;
        float phase = fullTurn * (uv.y * grid * 0.5 + uv.x + float(stepIndex % 8) / 8.0);
        vec2 sway = vec2(sin(phase), cos(phase * 0.75)) * vec2(0.19, 0.12) * strength;
        vec2 point = fract((uv - 0.5) / (1.0 + 0.65 * strength) + 0.5 + sway);
        vec4 warped = sampleInput(point);
        vec4 split = sampleInput(fract(point + vec2(0.025 * strength, 0.0)));
        // Transfer straight color into warped coverage to retain premultiplied alpha.
        float red = split.a > 0.0 ? split.r / split.a * warped.a : 0.0;
        warped.r = mix(warped.r, red, strength);
        fragColor = mix(original, warped, strength);
    } else {
        int sourceIndex = shuffleIndex(tile.y * gridSize + tile.x, gridSize * gridSize);
        ivec2 sourceTile = ivec2(sourceIndex % gridSize, sourceIndex / gridSize);
        vec2 flipped = local;
        if ((sourceIndex + stepIndex) % 2 == 0) flipped.x = 1.0 - flipped.x;
        if ((sourceIndex + stepIndex) % 3 == 0) flipped.y = 1.0 - flipped.y;
        vec2 zoomed = (flipped - 0.5) / (1.0 + 0.8 * strength) + 0.5;
        vec4 shuffled = sampleInput((vec2(sourceTile) + zoomed) / grid);
        float seam = step(0.04, min(min(local.x, 1.0 - local.x), min(local.y, 1.0 - local.y)));
        shuffled.rgb *= mix(1.0, seam, strength * 0.85);
        fragColor = mix(original, shuffled, strength);
    }
}
)";
