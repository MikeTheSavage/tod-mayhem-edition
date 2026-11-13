// suave_msgs.h - every script message the Suave code sends: MSG(constant, receiver class, negative id, command).
// A negative id -n addresses entry n-2 of the receiver class's method table (sorted by command id); the id must be the
// FIRST entry of its command's group (Scriptbaked::DispatchMessage 0x48A7E0 reads the handler count there).
// suave/code/tools/check_suave.py checks every row against the class registry (classes.json
// from the author's reverse-engineering notes, the 399 baked class registrations) and that each constant is used with that receiver.
// Included twice by suave_game.cpp; no include guard on purpose.
#ifndef MSG
#define MSG(name, cls, id, cmd)
#endif
MSG(MSG_TIME_SLOWMO,       "master_control_time_ctrl", -8,  "command_trigger_slowmotion(number,number,truth)")
MSG(MSG_TIME_EXPLOSION,    "master_control_time_ctrl", -13, "command_do_explosion(number)")
MSG(MSG_GOD_INDUCE,        "character_goodie_ctrl",    -8,  "command_induce_god_mode(number)")
MSG(MSG_GOODIE_ACTIVATE,   "character_goodie_ctrl",    -7,  "activate_goodie(integer,number)")
MSG(MSG_HUD_START,         "HUD_power_up_effects_ctrl", -8, "command_start_effect(integer,number)")
MSG(MSG_HUD_END,           "HUD_power_up_effects_ctrl", -4, "command_end_effect")
MSG(MSG_PLAY_SPEAK,        "camera_sound_ctrl",        -15, "command_Play_Speak(Entity,Entity)")
MSG(MSG_STOP_SPEAK,        "camera_sound_ctrl",        -16, "command_stop_speak(Entity)")
MSG(MSG_KILL_COMBO_MUSIC,  "camera_sound_ctrl",        -22, "command_kill_combo_streams")
MSG(MSG_PAUSE_MISSION_SOUND,"camera_sound_ctrl",       -26, "command_pause_mission_sound_emitters")
MSG(MSG_RESUME_MISSION_SOUND,"camera_sound_ctrl",      -27, "command_resume_mission_sound_emitters")
MSG(MSG_MOVE_NEW_STATE,    "character_animation_ctrl", -36, "Request_new_State(integer,integer)")
MSG(MSG_MOVE_IDLE,         "character_animation_ctrl", -81, "command_idle")
MSG(MSG_MOVE_WALK,         "character_animation_ctrl", -69, "command_walk")
MSG(MSG_MOVE_STOP,         "character_animation_ctrl", -5,  "command_stop_moving")
MSG(MSG_MOVE_TELEPORT,     "character_animation_ctrl", -19, "command_teleport_here(vector)")
MSG(MSG_AI_NO_CONTROL,     "character_sentry_ai",      -23, "command_no_ai_control")
MSG(MSG_AI_REGAIN,         "character_sentry_ai",      -9,  "command_regain_control")
MSG(MSG_ROOT_TAKE_A_HIT,   "character_root",           -2,  "take_a_hit(number,entity,entity,entity,vector,vector,integer)")
MSG(MSG_WEAPON_SPECIAL,    "character_weapon_ctrl",    -66, "command_goto_special_move_state")
MSG(MSG_WEAPON_SPECIAL_END,"character_weapon_ctrl",    -42, "special_move_ended")
MSG(MSG_WEAPON_UNARMED,    "character_weapon_ctrl",    -13, "command_switch_to_unarmed")
MSG(MSG_WEAPON_BEST,       "character_weapon_ctrl",    -37, "command_switch_to_best_weapon")
MSG(MSG_WEAPON_DROP,       "character_weapon_ctrl",    -24, "command_force_drop_weapon(Entity)")
MSG(MSG_PHYS_NO_COLLISION, "character_physic_ctrl",    -17, "command_disable_collision")
MSG(MSG_PHYS_COLLISION,    "character_physic_ctrl",    -11, "command_enable_collision")
MSG(MSG_PHYS_RESET,        "character_physic_ctrl",    -22, "command_reset_to_parent")
MSG(MSG_SHELL_EXPLODE,     "weapon_shell",             -9,  "command_explode(truth,truth,vector)")
MSG(MSG_SHELL_MORPH,       "weapon_shell",             -11, "command_morph(integer)")
MSG(MSG_SHELL_AIM,         "weapon_shell",             -12, "command_aim(vector)")
MSG(MSG_SHELL_INACTIVE,    "weapon_shell",             -14, "command_goto_inactive")
MSG(MSG_GUN_KILL_EFFECTS,  "default_weapon",           -8,  "command_kill_effects")
MSG(MSG_GUN_DROP,          "default_weapon",           -11, "command_drop")
MSG(MSG_GUN_STOP_FIRING,   "default_weapon",           -12, "command_stop_firing")
MSG(MSG_FACE,              "character_face_ctrl",      -3,  "command_facial_expression(integer,number,truth)")
MSG(MSG_CAM_SHAKE,         "camera_basic_ai",          -6,  "command_shake_cam(number,number,Entity)")
MSG(MSG_CAM_LOOK_AT,       "camera_basic_ai",          -7,  "command_look_at_target(Entity,vector)")
MSG(MSG_CAM_TRANSITION,    "camera_basic_ai",          -8,  "command_transition_to(Entity,Entity,number,number,number)")
MSG(MSG_CAM_CUT_TO,        "camera_basic_ai",          -9,  "command_cut_to(Entity,Entity)")
MSG(MSG_CAM_STOP_LOOK,     "camera_basic_ai",          -10, "command_stop_look_at_target")
MSG(MSG_CAM_BT_ON,         "camera_basic_ai",          -17, "command_goto_BulletTime_handling")
MSG(MSG_CAM_BT_OFF,        "camera_basic_ai",          -18, "command_leave_BulletTime_handling")
MSG(MSG_FOV,               "camera_root",              -4,  "command_change_fov(number,number)")

// native method indices / builtin global ids (not class-table messages; evidence in dll/HOOKS.md)
#ifndef SUAVE_NATIVES
#define SUAVE_NATIVES
static const int NAT_MOTION = 0x5E;     // MotionLayer motion(entity,integer) via Node::CallNative 0x86A250 (127 sites)
static const int NAT_SETPLAYPOS = 0x4B; // setplaypos(number) (inferred: registration order + the 1.0 call at 0x548130)
static const int NAT_ATTACH = 0x45, GID_ATTACH = 0x56;   // attachnode(int, Node*, char* pivot, char* bone) 0x6140F8
static const int NAT_DETACH = 0x46, GID_DETACH = 0x57;   // detachnode(int)
#endif
