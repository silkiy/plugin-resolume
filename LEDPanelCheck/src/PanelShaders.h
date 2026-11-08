#pragma once

inline constexpr char panelVertexShader[] = R"(#version 410 core
layout(location = 0) in vec3 position;

void main()
{
    gl_Position = vec4(position, 1.0);
}
)";

inline constexpr char panelFragmentShader[] = R"(#version 410 core
out vec4 fragColor;

uniform ivec2 viewportOrigin;
uniform ivec2 viewportSize;
uniform ivec2 cabinetSize;
uniform int pattern;

const vec3 darkFill = vec3(0.026, 0.060, 0.090);
const vec3 lightFill = vec3(0.045, 0.096, 0.133);
const vec3 gridInk = vec3(0.18, 0.74, 0.84);
const vec3 arrowInk = vec3(1.0, 0.69, 0.22);
const vec3 labelInk = vec3(0.95, 0.98, 1.0);

// Five rows of three pixels, packed from top-left to bottom-right.
const int digitBits[10] = int[10](
    (7 << 12) | (5 << 9) | (5 << 6) | (5 << 3) | 7, // 0: 111 101 101 101 111
    (2 << 12) | (6 << 9) | (2 << 6) | (2 << 3) | 7, // 1: 010 110 010 010 111
    (7 << 12) | (1 << 9) | (7 << 6) | (4 << 3) | 7, // 2: 111 001 111 100 111
    (7 << 12) | (1 << 9) | (7 << 6) | (1 << 3) | 7, // 3: 111 001 111 001 111
    (5 << 12) | (5 << 9) | (7 << 6) | (1 << 3) | 1, // 4: 101 101 111 001 001
    (7 << 12) | (4 << 9) | (7 << 6) | (1 << 3) | 7, // 5: 111 100 111 001 111
    (7 << 12) | (4 << 9) | (7 << 6) | (5 << 3) | 7, // 6: 111 100 111 101 111
    (7 << 12) | (1 << 9) | (1 << 6) | (1 << 3) | 1, // 7: 111 001 001 001 001
    (7 << 12) | (5 << 9) | (7 << 6) | (5 << 3) | 7, // 8: 111 101 111 101 111
    (7 << 12) | (5 << 9) | (7 << 6) | (1 << 3) | 7  // 9: 111 101 111 001 111
);

bool labelPixel(ivec2 glyphPixel, int number, int digits)
{
    int digitIndex = glyphPixel.x / 4;
    int column = glyphPixel.x % 4;
    if (column == 3) {
        return false;
    }
    for (int index = digitIndex + 1; index < digits; ++index) {
        number /= 10;
    }
    int bitIndex = (4 - glyphPixel.y) * 3 + 2 - column;
    return ((digitBits[number % 10] >> bitIndex) & 1) != 0;
}

bool inside(ivec2 point, ivec2 size)
{
    return all(greaterThanEqual(point, ivec2(0))) && all(lessThan(point, size));
}

vec3 cabinetMap(ivec2 pixel)
{
    ivec2 size = max(cabinetSize, ivec2(1));
    ivec2 cell = pixel / size;
    ivec2 origin = cell * size;
    ivec2 local = pixel - origin;
    ivec2 visible = min(size, viewportSize - origin);

    int markerLength = clamp(min(visible.x, visible.y) / 8, 3, 12);
    if (all(equal(cell, ivec2(0))) &&
        ((local.x < 2 && local.y < markerLength) || (local.y < 2 && local.x < markerLength))) {
        return arrowInk;
    }
    if (local.x == 0 || local.y == 0 || any(equal(pixel, viewportSize - 1))) {
        return gridInk;
    }

    int columns = (viewportSize.x - 1) / size.x + 1;
    int number = cell.y * columns + cell.x + 1;
    int digits = 1;
    for (int remaining = number; remaining >= 10; remaining /= 10) {
        ++digits;
    }

    int labelWidth = digits * 4 - 1;
    ivec2 available = visible * 3 / 4;
    int scale = min(available.x / max(labelWidth, 5), available.y / 12);
    bool hasArrow = scale > 0;
    if (!hasArrow) {
        scale = min(available.x / labelWidth, available.y / 5);
    }
    if (scale > 0) {
        int contentHeight = hasArrow ? 12 : 5;
        int top = (visible.y - contentHeight * scale) / 2;
        ivec2 labelOffset = ivec2((visible.x - labelWidth * scale) / 2, top + (hasArrow ? 7 * scale : 0));
        ivec2 label = local - labelOffset;
        if (inside(label, ivec2(labelWidth, 5) * scale) && labelPixel(label / scale, number, digits)) {
            return labelInk;
        }
        ivec2 arrow = local - ivec2((visible.x - 5 * scale) / 2, top);
        if (hasArrow && inside(arrow, ivec2(5 * scale))) {
            ivec2 arrowPixel = arrow / scale;
            int halfWidth = arrowPixel.y < 3 ? arrowPixel.y : 0;
            if (abs(arrowPixel.x - 2) <= halfWidth) {
                return arrowInk;
            }
        }
    }

    return ((cell.x + cell.y) & 1) == 0 ? darkFill : lightFill;
}

void main()
{
    ivec2 pixel = ivec2(floor(gl_FragCoord.xy)) - viewportOrigin;
    pixel.y = viewportSize.y - 1 - pixel.y;
    if (pattern == 0) {
        fragColor = vec4(cabinetMap(pixel), 1.0);
    } else {
        int line = pattern == 2 ? pixel.y : pattern == 3 ? pixel.x : pixel.x + pixel.y;
        float level = float(1 - (line & 1));
        fragColor = vec4(vec3(level), 1.0);
    }
}
)";
