"""make_ini.py: the Gameplay+ tod_hd_fx.ini, generated from the TOD HD release ini so a re-sync is one command.

Every key of the release ini is written, in the release's order, with its own one-line comment:
  * the HD effects that need no new art (group A) keep the TOD HD value, or the Gameplay+ value in GP_VALUES;
  * the display keys Gameplay+ owns ([display] and the in-game resolution switch) are OFF (DISPLAY_OFF);
  * every feature that needs TOD HD's art (group B) is set to its OFF value (B_OFF), because the DLL's built-in
    default for a key missing from the ini is often ON (FxDefaults: sky_dome, shine, env, veg, parallax ...).
Keys a level look would add (`map06.` lines) are dropped.

    python -I make_ini.py [--release DIR] [--out FILE]
"""
import argparse
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
HDFX = os.path.dirname(HERE)

# Gameplay+ owns the window, DPI and the resolution: the TOD HD versions stay off (hdfx/README.md "Display")
DISPLAY_OFF = {"borderless": "0", "dpi_aware": "0", "resolution_lock": "0"}

# features that need TOD HD's own art (textures, models, HDRIs, sheets, scans, the rebake): OFF
B_OFF = {
    "env": "0", "shine": "0", "sun_shadows": "0", "part_flipbook": "0", "part_flipbook_once": "0",
    "explosion_fx": "0", "sky_dome": "0", "sky_sets": "0", "sun_disc": "0", "local_reflections": "0",
    "terrain_rock": "0", "yard_decals": "0", "veg": "0", "veg_grass": "0", "veg_levels": "0",
    "veg_packs": "0", "veg_scatter_sub": "0", "veg_cards": "0", "parallax": "0", "tex_override": "0", "tex_pbr": "0",
    "replacement_models": "0", "tex_streaming": "0", "clearcoat": "0", "vehicle_hd": "0", "ambient_warm": "0",
    "level_looks": "0",
}

# Gameplay+ values that differ from the TOD HD release ini (group A). The sub-toggles of the installer only flip the
# on/off keys (installer/gpi/plan.py HDFX_SUBS); everything else here is a tuning value.
GP_VALUES = {
    "part_lit": "1",            # TOD HD ships 0; the TOD HD player's own ini has it on
    "heat_haze": "1",           # same
    "volumetric_beams": "1",    # same
    "volumetric_light": "1",    # same
    "terrain_triplanar": "1",   # same; it re-projects the game's own texture on steep terrain
    "water_mode": "1",          # same; its two textures are procedural (TOD HD tools\make_water_tex.py), no art
    "dyn_shadows": "1",         # same: characters, weapons and vehicles cast the level's sun
    "dyn_shadow_select": "1",   # same (the popfix)
    "emissive_select": "1",     # same (only acts at lighting_mode = 2)
    "hbao_intensity": "2.00",
    # the grade: TOD HD's (exposure 0.18, contrast 1.10, saturation 1.08, warmth 0.12, vignette 0.08, lift -0.010) was
    # tuned on its HD textures and HDRI sky; on the vanilla textures, whose sunsets are already warm, it pushes the
    # picture orange (compared side by side in game). A lighter grade:
    "exposure": "0.08", "contrast": "1.06", "saturation": "1.04", "warmth": "0.0", "vignette": "0.05", "lift": "-0.005",
}

HEADER = """; ============================================================================================
;  Gameplay+ HD effects -- settings for d3d9.dll (the TOD HD effects DLL, used as released).
;  Read from the game folder; re-read about once a second while the game runs, so a value can be
;  tuned live.  Lines starting with ; or # are comments.
;
;  What is ON here needs no new art: texture filtering, ambient occlusion, the post pass (grade,
;  sharpen, FXAA), sun shafts and mist, the particle fixes, per-pixel sun lighting from the game's
;  own sky, real-time sun shadows of characters and vehicles, the water shader and the frame cap.
;  Everything that needs the TOD HD texture / model / sky art is OFF at the end of this file and
;  has no effect even if switched on (its data is not installed).
;
;  Gameplay+ owns the window and the resolution ([display] in TODGameplay.ini and the in-game
;  SETTINGS), so borderless, dpi_aware and resolution_lock are 0 here.
;
;  Turn the effects off with the Gameplay+ installer (HD Effects group off): it removes d3d9.dll,
;  this file and tod_hd_fx\\.  `enabled = 0` is a pass-through (a test aid).
;  Logs: %LOCALAPPDATA%\\TODGameplay\\hdfx_logs (set by the Gameplay+ dsound.dll loader; [loader]
;  hdfx_log_dir in TODGameplay.ini changes it; never the game folder).
;  F8 flips the post pass on/off while playing (post_toggle_key).
; ============================================================================================
"""

B_HEADER = """
; ============================================================================================
;  TOD HD features that need its own art (HD textures, the forest, height maps, rock scans,
;  flipbook sheets, the HDRI sky and reflections, decal images, the lighting rebake).  Not
;  installed by Gameplay+, so they stay OFF.  Their tuning keys are kept so the DLL never falls back
;  to a default.
; ============================================================================================
"""

KEYLINE = re.compile(r"^\s*([a-z_0-9.]+)\s*=\s*([^;]*?)\s*(;.*)?$")


def first_sentence(comment):
    c = (comment or "").lstrip(";").strip()
    c = re.sub(r"C:\\TOD HD[^\s,)]*", "", c)
    m = re.match(r"(.{0,110}?[.)])(\s|$)", c)
    c = m.group(1) if m else c[:110]
    return c.strip()


def is_b(key):
    if key in B_OFF:
        return True
    pre = ("veg_", "tex_", "rock_", "yard_decals", "sky_", "sun_disc", "sun_glow", "clearcoat", "ambient_",
           "local_reflections", "part_flipbook", "explosion_", "shine_", "env_", "sun_shadow", "height_", "steps_",
           "mip_fade", "fade_", "min_cos", "grazing_fade", "max_offset", "reference", "cavity", "toggle_key",
           "debug", "replacement_models")
    keep = ("sky_depth", "sky_distance", "sky_intensity", "sky_colour_r", "sky_colour_g", "sky_colour_b",
            "sky_bake_occ", "sky_scope")
    if key in keep:
        return False
    if key.startswith("tex_"):
        return True
    return key.startswith(pre)


def build(release_ini):
    lines = open(release_ini, encoding="utf-8", errors="replace").read().splitlines()
    a_out, b_out, seen = [], [], set()
    section = None
    for ln in lines:
        s = ln.strip()
        if s.startswith("; ----") and s.endswith("-"):
            section = s
            continue
        m = KEYLINE.match(ln)
        if not m or s.startswith(";") or s.startswith("#"):
            continue
        key, val, com = m.group(1), m.group(2), m.group(3)
        if "." in key or key in seen:
            continue
        seen.add(key)
        if key in DISPLAY_OFF:
            val, com = DISPLAY_OFF[key], "; Gameplay+ owns this ([display] / SETTINGS): off here"
        elif key in B_OFF:
            val = B_OFF[key]
        elif key in GP_VALUES:
            val = GP_VALUES[key]
        text = "%-24s ; %s" % ("%s = %s" % (key, val), first_sentence(com)) if first_sentence(com) else "%s = %s" % (key, val)
        if key == "lighting_mode":
            section = "; ---- per-pixel lighting: the DLL adds the level's own sun per pixel (mode 1) -"
        if is_b(key) and key not in DISPLAY_OFF:
            b_out.append(text)
        else:
            if section and (not a_out or a_out[-1] != "") and section != getattr(build, "_last", None):
                title = section.strip("; -").strip()
                a_out += ["", "; ---- %s" % title]
                build._last = section
            a_out.append(text)
    return HEADER + "\n".join(a_out).lstrip("\n") + "\n" + B_HEADER + "\n".join(b_out) + "\n", seen


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--release", default=os.path.join(HDFX, "release", "tod_hd_fx.ini"))
    ap.add_argument("--out", default=os.path.join(HDFX, "payload", "tod_hd_fx.ini"))
    a = ap.parse_args(argv)
    text, keys = build(a.release)
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("written %s: %d keys" % (a.out, len(keys)))


if __name__ == "__main__":
    main()
