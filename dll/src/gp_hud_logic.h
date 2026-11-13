// gp_hud_logic.h - pure logic of [minimap] (no game memory, no Direct3D; unit tested by tests/dll/test_hud.cpp): the
// game's radar frame, the minimap disc geometry and texture coordinates, the world projection of objective markers
// with the screen-edge arrow, the objective blip types, distance text and the baked-map index.
#pragma once
#include <stdint.h>

namespace hud {

struct V2 { float x, y; };
struct V3 { float x, y, z; };

// ---------------------------------------------------------------- the radar (HUD_radar, see dll/HOOKS.md "Minimap")
// The game places a blip at (x, y) = BG pos + (64, 68) - blip half + Factor * (side, -forward) with Factor = 54 / R
// (R = RadarRadius 50 m) and the target clamped to R, in the frame of the player's view (PlayerView, y = 0). The
// radar texture's parchment disc: centre (64.5, 68) of 128 x 128, radius 52.5.
const float RADAR_TEX = 128.0f, DISC_CX = 64.5f, DISC_CY = 68.0f, DISC_R = 52.5f, BLIP_R = 54.0f;
const float GAME_RADIUS_M = 50.0f;

struct Frame {             // the radar's frame in world space: forward f = (fx, fz) (unit, xz plane), right = (fz, -fx)
    float fx = 0.0f, fz = 1.0f;
    float px = 0.0f, pz = 0.0f;   // the player (radar centre)
};
// a horizontal world offset (dx, dz) -> radar units (x right, y down on screen), not clamped
V2 to_radar(const Frame& f, float dx, float dz, float units_per_m);
// radar units (x right, y down) -> world offset (dx, dz)
V2 from_radar(const Frame& f, float ux, float uy, float units_per_m);
// heading-up frame from the game's PlayerView; north-up frame (forward = +z)
Frame frame_from_view(float vx, float vz, float px, float pz);
Frame frame_north(float px, float pz);
// rotate a game blip position (radar units relative to the centre, view frame) into the north-up frame
V2 view_to_north(const Frame& view, float ux, float uy);

// the radar background quad on screen (4 corners as the sprite emits them, any order of TL/BL/BR/TR is resolved by the
// UVs) -> the disc centre and radius in pixels and the size of one radar unit in pixels
struct Disc { float cx, cy, r, unit; bool ok; };
Disc disc_from_quad(const float P[4][2], const float UV[4][2]);

// ---------------------------------------------------------------- the baked map
struct MapInfo { char name[48]; char file[64]; float x0, z0, x1, z1; int w, h; bool pack; };   // world rect (x0 < x1, z0 < z1) -> image
// one line: name x0 z0 x1 z1 width height file  ('#' comments); false when malformed. file *.mmp = a tile pack
// (gp_hud_tile.h; w x h = the whole map at the finest level), anything else = the one picture of an older bake
bool parse_map_line(const char* line, MapInfo* out);
// world (x, z) -> texture (u, v): u = (x - x0) / (x1 - x0), v = (z1 - z) / (z1 - z0) (image row 0 = north = max z)
V2 map_uv(const MapInfo& m, float x, float z);
// the index of the map whose rect holds (x, z) best (deepest inside, smallest when nested); -1 none
int pick_map(const MapInfo* maps, int n, float x, float z);
// which cells of a map hold collision (<name>.occ of the bake: 'occ <cell> <cols> <rows> <x0> <z1>' + rows of 0/1, north
// first). The city maps share one world and their rectangles overlap: the map with ground under the player wins.
struct Occ { float cell = 0, x0 = 0, z1 = 0; int cols = 0, rows = 0; unsigned char* bits = nullptr; };
bool parse_occ(const char* text, Occ* o);            // allocates o->bits (new[]); false when malformed
bool occ_near(const Occ& o, float x, float z);       // a '1' cell at (x, z) or next to it
int pick_map_occ(const MapInfo* maps, const Occ* occ, int n, float x, float z);   // occupancy first, then the rectangles

// ---------------------------------------------------------------- objective blips
// blip types of the radar (texture list order, HUD_radar init 0x6F1CA0): 0 SubMission, 1 StoryMission, 2 Waypoint,
// 3 Enemy, 4 Ally, 5 Neutral, 6 Blowup, 7 Use, 8 Boss, 9 Enemy_Obj, 10 Ally_Obj, 11 Default, 12 Prim_Gate, 13 Prim_Gate
// (secondary, drawn at 25 %), 14 Tutorial_Area
bool is_objective(int type);              // what a mission marks as the thing to go to / deal with
int objective_rank(int type);             // lower = more important (the world marker with the distance first)
uint32_t objective_colour(int type);      // ARGB of the marker
// "86 m", "950 m", "1.2 km"; returns the length
int format_distance(float metres, char* out, int cap);

// ---------------------------------------------------------------- world projection (the game's camera view 0)
// camera-to-world matrix rows (right, up, forward, position), fov = the view's horizontal 4:3 fov in degrees, ratioxy,
// the viewport size in pixels; widescreen_flag = the console 16:9 mode (aspect fixed 16/9)
struct Cam { float m[16]; float fov_deg, ratioxy, W, H; bool wide_flag; };
// world -> pixels; false when behind the camera (z <= near) - then x/y hold the direction mirrored to the front
bool project(const Cam& c, V3 p, float* sx, float* sy, float* depth);
// a point that must be shown on screen: inside the margin it stays, otherwise it goes to the border (inset by margin)
// along the ray from the screen centre; *edge = true when moved; *angle = the arrow's direction (radians, 0 = right,
// screen y down). behind = the target is behind the camera (the direction is flipped first)
V2 clamp_to_screen(float sx, float sy, bool behind, float W, float H, float margin, bool* edge, float* angle);

}  // namespace hud
