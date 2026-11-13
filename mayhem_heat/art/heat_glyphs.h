// heat_glyphs.h - written by mayhem_heat/art/make_heat_art.py: the layout of heat_icons.png (atlas pixels).
#pragma once
// the chili icons: cell i (0 = cold, 1..5 = flame level) at x = i * HEAT_CELL, y = 0; the full square source
// art is HEAT_ICON px inset HEAT_PAD px in its cell (the margin holds the drop shadow)
static const int HEAT_ATLAS_W = 1024, HEAT_ATLAS_H = 512;
static const int HEAT_CELL = 160, HEAT_ICON = 128, HEAT_PAD = 16;
// the pepper's box centre in the square art (fractions): the DLL puts it on the slot centre
static const float HEAT_PEPPER_U = 0.4968f, HEAT_PEPPER_V = 0.6061f;
// the edge ring: 256 x 256 = radar UV 0..1
static const int HEAT_RING_X = 0, HEAT_RING_Y = 160, HEAT_RING_N = 256;
// the score multiplier label figures: {char, x, y, w, h, advance}; every cell has its baseline at 80 % of h
struct HeatGlyph { char c; short x, y, w, h, adv; };
static const HeatGlyph HEAT_GLYPHS[12] = {
    {'x', 256, 160, 35, 64, 26},
    {'0', 298, 160, 36, 64, 27},
    {'1', 340, 160, 30, 64, 21},
    {'2', 382, 160, 39, 64, 30},
    {'3', 424, 160, 36, 64, 27},
    {'4', 466, 160, 37, 64, 28},
    {'5', 256, 224, 38, 64, 29},
    {'6', 298, 224, 36, 64, 27},
    {'7', 340, 224, 36, 64, 27},
    {'8', 382, 224, 36, 64, 27},
    {'9', 424, 224, 36, 64, 27},
    {'.', 466, 224, 21, 64, 12},
};

