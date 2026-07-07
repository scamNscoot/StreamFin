/*
    StreamFin ocean theme — shared palette + background painter.

    Stremio-style look: a deep ocean-navy vertical gradient with a soft purple
    wash from the top and a blue tide from the bottom corner, plus the purple
    primary accent used across the stremio screens.
*/
#pragma once

#include <borealis.hpp>

namespace stremio_theme {

const NVGcolor ACCENT   = nvgRGB(123, 91, 245);   // Stremio purple (buttons, meta)
const NVGcolor ACCENT_HI = nvgRGB(157, 131, 255); // lighter purple (text on dark)
const NVGcolor TEXT     = nvgRGB(240, 242, 248);  // soft white
const NVGcolor TEXT_DIM = nvgRGB(168, 176, 192);  // secondary text

// Paint the full-screen background: dark ocean blue fading to near-black,
// blended with a purple glow at the top and a blue one at the bottom-left.
inline void drawOceanBackground(NVGcontext* vg, float x, float y, float width, float height) {
    // Base: deep navy -> near-black.
    NVGpaint base = nvgLinearGradient(vg, x, y, x, y + height, nvgRGB(17, 23, 49), nvgRGB(5, 7, 15));
    nvgBeginPath(vg);
    nvgRect(vg, x, y, width, height);
    nvgFillPaint(vg, base);
    nvgFill(vg);

    // Purple wash bleeding in from above the top edge.
    NVGpaint wash = nvgRadialGradient(vg, x + width * 0.5f, y - height * 0.25f, height * 0.2f, height * 1.1f,
        nvgRGBA(101, 70, 220, 46), nvgRGBA(101, 70, 220, 0));
    nvgBeginPath(vg);
    nvgRect(vg, x, y, width, height);
    nvgFillPaint(vg, wash);
    nvgFill(vg);

    // Ocean-blue tide rising from the bottom-left corner.
    NVGpaint tide = nvgRadialGradient(vg, x, y + height * 1.15f, height * 0.2f, height * 0.9f,
        nvgRGBA(30, 90, 200, 30), nvgRGBA(30, 90, 200, 0));
    nvgBeginPath(vg);
    nvgRect(vg, x, y, width, height);
    nvgFillPaint(vg, tide);
    nvgFill(vg);
}

}  // namespace stremio_theme
