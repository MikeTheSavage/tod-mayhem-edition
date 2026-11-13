// car_glue.h - the hooks [physics] (phys_game.cpp) calls into [car_physics] (car_game.cpp). Every function is a no-op
// while [car_physics] enabled = 0 or the car references differ. Cars need the Box3D world of [physics], so
// [car_physics] runs only together with [physics] enabled = 1.
#pragma once
#include <windows.h>
#include "phys_api.h"

void car_load_config();                                  // phys_load_config: reads [car_physics]
bool car_install(const PhApi* api, HMODULE physics_dll);  // phys_install, after TODPhysics.dll loaded
bool car_active();                                       // installed and enabled
bool car_blasts_explosions();                            // installed and blast_cars: a car explosion is blasted here ([mayhem] skips its own)
float car_prop_max_radius(const char* name, float def);   // prop size rule for the extra_props names
const char* car_extra_props();                           // [car_physics] extra_props (only when enabled): more [physics] props
// scan (phys_game.cpp scan()): every node walked; a vehicle model is skipped as static level (it gets a kinematic box)
void car_scan_node(DWORD node);
bool car_scan_skip_model(DWORD model);
void car_after_scan();                                   // the world was rebuilt: boxes / wrecks again
// per frame (phys_tick), in this order
void car_frame(double now, float game_dt);               // car list, poses, explosions of cars, wreck take-over
void car_before_step(int i, int n, float step);          // kinematic sweep targets
void car_after_step();                                   // prop kicks from first touches, wreck poses
void car_after_frame(float alpha);                       // wreck write-back
void car_rewind_end();                                   // after a rewind: everything back where the game has it
// blasts and bullets ([physics] hooks PH_SHELL / PH_DESTROY / PH_BULLET)
void car_blast(float cx, float cy, float cz, float radius, float impulse_per_area, float lift, const char* who);
void car_bullet(const float from[3], const float to[3], float bullet_impulse);
// keys and statistics
void car_post_input(const BYTE* kb_edges, const BYTE* kb_state, double now);
void car_log_stats();
