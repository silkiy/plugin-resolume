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

void main()
{
    vec4 original = sampleInput(uv);
    float strength = amount * pulse;
    if (strength <= 0.0) {
        fragColor = original;
        return;
    }

    if (pattern == 3) {
        // Lift the current content toward premultiplied white without changing alpha.
        fragColor = vec4(mix(original.rgb, vec3(original.a), 0.25 * strength), original.a);
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
        float stagger = float(tile.x + 2 * tile.y + stepIndex % gridSize) / grid;
        float phase = fullTurn * (stagger + 0.5 * (1.0 - pulse));
        vec2 sway = vec2(sin(phase), cos(phase)) * vec2(0.26, 0.14) * strength;
        fragColor = sampleInput((vec2(tile) + clamp(local + sway, 0.0, 1.0)) / grid);
    } else {
        int sourceIndex = shuffleIndex(tile.y * gridSize + tile.x, gridSize * gridSize);
        ivec2 sourceTile = ivec2(sourceIndex % gridSize, sourceIndex / gridSize);
        fragColor = mix(original, sampleInput((vec2(sourceTile) + local) / grid), strength);
    }
}
)";
