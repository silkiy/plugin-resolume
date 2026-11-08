#pragma once

inline constexpr char timerVertexShader[] = R"(#version 410 core
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 texCoord;
out vec2 uv;

void main()
{
    gl_Position = vec4(position, 1.0);
    uv = texCoord;
}
)";

inline constexpr char timerFragmentShader[] = R"(#version 410 core
in vec2 uv;
out vec4 fragColor;
uniform ivec4 digits;
uniform float viewportAspect;
uniform vec3 digitColor;

const float digitRadius = 0.057;
const float outerDigit = 1.08;
const float innerDigit = 0.43;
const float colonOffset = 0.145;

// Segments run clockwise from the top, with the middle bar last.
const int digitMasks[10] = int[10](63, 6, 91, 79, 102, 109, 125, 7, 127, 111);
const vec2 segmentStart[7] = vec2[7](
    vec2(-0.16,  0.44), vec2( 0.228,  0.115), vec2( 0.228, -0.326),
    vec2(-0.16, -0.44), vec2(-0.228, -0.326), vec2(-0.228,  0.115),
    vec2(-0.16,  0.00)
);
const vec2 segmentEnd[7] = vec2[7](
    vec2( 0.16,  0.44), vec2( 0.228,  0.326), vec2( 0.228, -0.115),
    vec2( 0.16, -0.44), vec2(-0.228, -0.115), vec2(-0.228,  0.326),
    vec2( 0.16,  0.00)
);

float roundedSegment(vec2 point, vec2 start, vec2 end)
{
    vec2 axis = end - start;
    vec2 offset = point - start;
    float along = clamp(dot(offset, axis) / dot(axis, axis), 0.0, 1.0);
    return length(offset - along * axis) - digitRadius;
}

float digitDistance(vec2 point, int digit)
{
    int mask = digitMasks[clamp(digit, 0, 9)];
    float distanceToDigit = 1.0;
    for (int segment = 0; segment < 7; ++segment) {
        if ((mask & (1 << segment)) != 0) {
            distanceToDigit = min(distanceToDigit,
                roundedSegment(point, segmentStart[segment], segmentEnd[segment]));
        }
    }
    return distanceToDigit;
}

void main()
{
    float aspect = max(viewportAspect, 0.001);
    float fit = max(0.61, 1.50 / aspect);
    vec2 point = (uv * 2.0 - 1.0) * vec2(aspect, 1.0) * fit;
    float edge = max(0.65 * abs(dFdy(point.y)), 0.0001);

    float distanceToGlyph;
    float x = abs(point.x);
    if (x > outerDigit + 0.30 + edge || abs(point.y) > 0.50 + edge) {
        fragColor = vec4(0.0);
        return;
    }
    if (x < 0.09) {
        distanceToGlyph = length(vec2(point.x, abs(point.y) - colonOffset)) - digitRadius;
    } else {
        bool outer = x >= (outerDigit + innerDigit) * 0.5;
        float center = outer ? outerDigit : innerDigit;
        int index = outer ? 0 : 1;
        if (point.x > 0.0) {
            index = 3 - index;
        }
        distanceToGlyph = digitDistance(vec2(point.x - sign(point.x) * center, point.y), digits[index]);
    }

    float alpha = 1.0 - smoothstep(-edge, edge, distanceToGlyph);
    // Resolume composites FFGL output with premultiplied alpha.
    fragColor = vec4(digitColor * alpha, alpha);
}
)";
