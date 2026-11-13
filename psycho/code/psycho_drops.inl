// psycho_drops.inl - elite drops ([psycho] elite_drops): an elite that dies leaves the loco move it used as a pickup, one of
// the game's own goodies. Included by psycho_game.cpp (it uses that file's game helpers). The pure decisions are in
// psycho_logic.cpp (ps::EliteBook and friends, unit tested); this file reads the game and makes the calls.
//
// The pickup is a REAL goodie: one of the five pooled prop_root goodies of DefaultGoodieRessources.fragment, taken from the
// game's master_control_resource_ctrl the way goodie_placeholder does it (0x68B3B0), but without a placeholder. From then on
// the game runs it: its model, glow and rotation (Goodie_Default command_reset), the trigger on Ram (character_weapon_ctrl
// sphere query, trig on the prop's AI), activate_goodie with the slot maximum (a full slot shakes the prop and leaves it),
// HUD_pickup_effect, the pickup sound, and the prop's own AutoRemoveTimer for the time limit.
// Evidence: dll/HOOKS.md "PSYCHO enemy loco moves", section elite drops.

// script cache / controllers
static const DWORD M_RESOURCE = 0x40;          // MASTER_CTRL data: ResourceCtrl (master_control_resource_ctrl)
static const DWORD RC_SUCCESS = 0x0C, RC_AVAILABLE = 0x10, RC_LIST = 0x14;
static const int   ST_DROP = 2;                // ResourceStatus of a drop (see spawn_prop)
static const int   RES_GOODIE = 5;             // ActualResourceType of a goodie prop (Sub_Type 2 -> 5, prop_root 0x7A14F0)
static const int   MSG_RES_DEPRECATED = -5;    // command_resource_deprecated(Entity)
static const int   MSG_RES_FREE = -10;         // command_free_resource(Entity)
static const int   MSG_RES_REQUEST = -13;      // command_request_resource(integer)
static const int   MSG_PROP_RESET = -4;        // prop_root command_reset
static const int   CMD_GIVEUP_RESOURCE = 0x2AB;   // placeholder command_giveup_resource (as 0x75EC9C: the pool takes a far prop back)
// prop_root data (member slot in the comment)
static const DWORD P_LOCKED = 0x18 /*4*/, P_PLACEHOLDER = 0x1C /*5*/, P_CTRL = 0x24 /*7*/, P_MOVE = 0x28 /*8*/, P_AI = 0x2C /*9*/, P_STATUS = 0x38 /*12*/, P_SUBTYPE = 0x5C /*0x15*/;
static const int   SLOT_P_STATUS = 12, SLOT_P_SUBTYPE = 0x15, SLOT_AI_TIMER = 0;
static const DWORD A_LIST_LISTS = 0xA3BDF0, A_LIST_RES = 0xA3BCE4;     // the list types of ResourceList (0x75FA60)
static const DWORD PA_TIMER = 0x04;            // prop_ai_ctrl data: AutoRemoveTimer

typedef void(__fastcall* Flags_t)(DWORD node, void* edx, DWORD flags);    // Node vtable +10h SetFlags, ret 4

struct Drop { bool used, taken; DWORD prop, ai; uint32_t key; int ability, slot, count0; ps::Vec3 pos; double t0, t_end, t_taken; };   // taken: kept for a rewind past the pickup
static ps::EliteBook g_book;
static Drop   g_drops[5];
static double g_drop_scan_t = 0.0, g_drop_sweep_t = 0.0, g_drop_clock = -1.0;
static bool   g_drop_back = false;             // the clock went back: look at everything once the rewind is over
static int    g_drops_made = 0, g_drops_taken = 0, g_drops_gone = 0, g_drops_failed = 0, g_drops_undone = 0;
static float  g_drop_lift = 0.75f;             // m above the floor: the level's goodies float 0.64-0.89 m above theirs ([drops] probe);
                                               // at 0 a drop was half sunk into the floor
static float  g_drop_steal = 20.0f;            // a level goodie at least this far from Ram may give its prop to a drop
static int    g_debug_drop_key = gp::KEY_NONE; // test copy: a drop 3 m ahead of Ram (probe; also a pool check)
static bool   g_debug_drop_walk = false;       // test copy: Ram turns to the nearest drop and walks to it
static volatile LONG g_debug_drop = 0;
static int    g_debug_walk_frames = 0;
static float  g_debug_walk_wait = 3.0f;

static int g_debug_trace, g_debug_rewinds, g_debug_ram_dies_on;
static float g_debug_ram_dies;
static double g_debug_die_t = -1.0;
static bool g_debug_died = false;
static void drops_load_config() {
    g_c.drops = b_cfg("elite_drops", true);
    g_c.drop_seconds = f_cfg("drop_seconds", g_c.drop_seconds);
    g_c.drop_max = gp_ini().geti("psycho", "drop_max", g_c.drop_max);
    g_c.drop_track_radius = f_cfg("drop_track_radius", g_c.drop_track_radius);
    g_c.drop_settle = f_cfg("drop_settle_seconds", g_c.drop_settle);
    g_c.drop_body_reach = f_cfg("drop_body_reach", g_c.drop_body_reach);
    g_c.drop_body_drop = f_cfg("drop_body_drop", g_c.drop_body_drop);
    g_c.drop_give_up = f_cfg("drop_give_up_seconds", g_c.drop_give_up);
    g_c.clamp();
    g_drop_lift = f_cfg("drop_lift", 0.75f);
    g_drop_steal = f_cfg("drop_steal_distance", 20.0f);
    g_debug_drop_key = gp_key_cfg("psycho", "debug_drop_key", "NONE");
    g_debug_drop_walk = b_cfg("debug_drop_walk", false);
    g_debug_walk_wait = f_cfg("debug_drop_walk_wait", 3.0f);
    g_debug_trace = gp_ini().geti("psycho", "debug_drop_trace", 0);
    g_debug_rewinds = gp_ini().geti("psycho", "debug_rewinds", 0);
    g_debug_ram_dies = gp_ini().getf("psycho", "debug_ram_dies", 0.0f);        // s after the first drop (or collection): a lethal hit to Ram
    g_debug_ram_dies_on = gp_ini().geti("psycho", "debug_ram_dies_on", 0);     // 0 drop made, 1 drop collected
}

static DWORD resource_ctrl() {
    DWORD cd = gp_cache_data();
    __try { DWORD m = cd ? RD32(cd + C_MASTER) : 0; DWORD md = m ? RD32(m + 0x18) : 0; return md ? RD32(md + M_RESOURCE) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void touch(DWORD node, int slot) { __try { ((Touch_t)(uintptr_t)P_PS_TOUCH)(node, nullptr, slot); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
// the game's show (0x62E110 under CurNode = player holder): Node SetFlags with bit 0 clear, refused while holder +88h is set
static bool node_show(DWORD node) {
    __try {
        DWORD h = RD32(A_PLAYER_HOLDER); DWORD d = h ? RD32(h + 0x18) : 0;
        if (!d || !node || RD32(d + 0x88)) return false;
        ((Flags_t)(uintptr_t)RD32(RD32(node) + 0x10))(node, nullptr, RD32(node + 0x28) & 0xFFE);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Ram's count of a power-up slot and its maximum (character_powerup_ctrl data +14h -> {counts, maxima}, 0x542DF0)
static int loco_count(int slot, int* maximum) {
    __try {
        DWORD pd = data(sub(ram_root(), R_POWERUP)); DWORD p = pd ? RD32(pd + 0x14) : 0; if (!p) return -1;
        DWORD k = list_stride(A_LIST_INT); if (!k) return -1;
        DWORD L0 = RD32(p), L1 = RD32(p + 4);
        if (maximum) *maximum = (int)RD32(RD32(RD32(L1 + 4) + 4) + (DWORD)slot * 4 * k);
        return (int)RD32(RD32(RD32(L0 + 4) + 4) + (DWORD)slot * 4 * k);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static int slot_of_move(int move) { return move == ps::MOVE_TORNADO ? 3 : 4; }   // Tornado, El Toro in Ram's power-up list

// the goodie props of the pool (ResourceList[5], as 0x75FA60 walks it)
static int pool_props(DWORD* out, int cap) {
    int n = 0;
    __try {
        DWORD rd = data(resource_ctrl()); if (!rd) return 0;
        DWORD tl = RD32(A_LIST_LISTS), tr = RD32(A_LIST_RES); if (!tl || !tr) return 0;
        DWORD k1 = RD32(tl + 0x24), k2 = RD32(tr + 0x24); if (!k1 || !k2) return 0;
        DWORD L = RD32(rd + RC_LIST); DWORD a = L ? RD32(L + 4) : 0; DWORD d = a ? RD32(a + 4) : 0; if (!d) return 0;
        DWORD inner = RD32(d + k1 * RES_GOODIE * 4); if (!inner) return 0;
        int c = (int)(RD32(inner + 8) / k2);
        DWORD b = RD32(inner + 4); DWORD e = b ? RD32(b + 4) : 0; if (!e) return 0;
        for (int i = 0; i < c && n < cap; ++i) { DWORD p = RD32(e + k2 * (DWORD)i * 4); if (p && RD32(p)) out[n++] = RD32(p); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return n; }
    return n;
}
static bool prop_state(DWORD prop, int* status, DWORD* placeholder) {
    __try { DWORD d = data(prop); if (!d) return false; *status = (int)RD32(d + P_STATUS); *placeholder = RD32(d + P_PLACEHOLDER); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool free_prop(DWORD prop) { DWORD p = prop; return send(resource_ctrl(), MSG_RES_FREE, &p); }

// ---- ground: where a line straight down from above the spot first hits (the Tornado's / Suave's line test, 2.5 cm steps)
static bool ground_y(const ps::Vec3& at, float* y, DWORD ignore) {
    ps::Vec3 top = at, bot = at; top.y += 1.0f; bot.y -= 3.0f;
    if (!blocked(top, bot, ignore)) return false;
    float lo = bot.y, hi = top.y;
    for (int i = 0; i < 8; ++i) { float mid = (lo + hi) * 0.5f; ps::Vec3 m = at; m.y = mid; if (blocked(top, m, ignore)) lo = mid; else hi = mid; }
    *y = hi; return true;
}

// The pool has five goodie props and the level's goodies take them first, nearest placements first. When none is free the
// level goodie furthest from Ram gives its prop back, what the pool itself does to a prop out of drawing range (0x75EC9C): its
// placeholder waits for a free prop and gets one again when a drop ends. Never one Ram could be looking at (drop_steal_distance).
static bool steal_prop(const ps::Vec3& ram) {
    DWORD props[16], best = 0, bph = 0; float bd = g_drop_steal; int n = pool_props(props, 16);
    for (int i = 0; i < n; ++i) {
        int st = 0; DWORD ph = 0; ps::Vec3 p;
        if (!prop_state(props[i], &st, &ph) || st != 1 || !ph || !get_pos(props[i], &p)) continue;
        __try { if (RD32(data(props[i]) + P_LOCKED)) continue; } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        float d = ps::dist_xz(p, ram);
        if (d > bd) { bd = d; best = props[i]; bph = ph; }
    }
    if (!best) return false;
    command(bph, CMD_GIVEUP_RESOURCE, nullptr);
    bool ok = free_prop(best);
    gp_log("psycho: goodie pool empty: the level goodie %.0f m from Ram gives its prop %08X back%s\n", bd, best, ok ? "" : " (FAILED)");
    return ok;
}

// a goodie of the pool becomes this drop; false = no prop free
static DWORD spawn_prop(int goodie, const ps::Vec3& p, float seconds) {
    DWORD rc = resource_ctrl(), rd = data(rc), prop = 0, pd = 0, ai = 0;
    if (!rd) return 0;
    int type = RES_GOODIE;
    if (!send(rc, MSG_RES_REQUEST, &type)) return 0;
    __try { if (!RD32(rd + RC_SUCCESS)) return 0; prop = RD32(rd + RC_AVAILABLE); pd = data(prop); ai = pd ? RD32(pd + P_AI) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!pd || !ai) return 0;
    // what command_allocate_resource does, without a placeholder: taken, shown. The status is 2, not the game's 1: a status 1
    // prop with no placeholder is "dangling" to the pool, which frees it within a second (0x75E9E0, 0x763D90). The game never
    // uses 2: requests want 0, the free paths take any status, so a status 2 prop is ours and left alone (the sweep below finds
    // any that a save brought back)
    __try { touch(prop, SLOT_P_STATUS); WR32(pd + P_STATUS, ST_DROP); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    bool shown = node_show(prop);
    // what goodie_placeholder's take-over does (0x68B3E0): sub type, then prop_root command_reset (model, glow, idle)
    __try { touch(prop, SLOT_P_SUBTYPE); WR32(pd + P_SUBTYPE, goodie); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    bool reset = send(prop, MSG_PROP_RESET, nullptr);
    set_pos(prop, p);
    __try { touch(ai, SLOT_AI_TIMER); WRF(data(ai) + PA_TIMER, seconds); } __except (EXCEPTION_EXECUTE_HANDLER) {}   // reset cleared it; idle reads it next
    if (g_verbose) gp_log("psycho: drop prop %08X: taken, shown %d, reset %d, flags %X\n", prop, shown ? 1 : 0, reset ? 1 : 0, RD32(prop + 0x28));
    return prop;
}

static bool make_drop(ps::Elite* e, int move, const ps::Vec3& at, double t, const ps::Vec3& ram, bool ram_ok) {
    Drop* d = nullptr; int active = 0;
    for (Drop& x : g_drops) { if (x.used && !x.taken) ++active; else if (!d || (d->used && !x.used)) d = &x; }
    if (!d || active >= g_c.drop_max) return false;
    DWORD prop = spawn_prop(ps::goodie_of_move(move), at, g_c.drop_seconds);
    if (!prop && ram_ok && steal_prop(ram)) prop = spawn_prop(ps::goodie_of_move(move), at, g_c.drop_seconds);
    if (!prop) return false;
    int slot = slot_of_move(move);
    d->used = true; d->taken = false; d->prop = prop; d->ai = sub(prop, P_AI); d->key = e ? e->key : 0; d->ability = move; d->slot = slot;
    d->count0 = loco_count(slot, nullptr); d->pos = at; d->t0 = t; d->t_end = t + g_c.drop_seconds;
    ++g_drops_made;
    int mx = -1, c = loco_count(slot, &mx);
    if (g_debug_ram_dies > 0.0f && !g_debug_ram_dies_on && g_debug_die_t < 0.0) g_debug_die_t = t + g_debug_ram_dies;
    gp_log("psycho: DROP %s at (%.2f %.2f %.2f): prop %08X, stays %.0f s (Ram holds %d of %d; %d on the ground)\n", ps::move_name(move), at.x, at.y, at.z, prop,
           g_c.drop_seconds, c, mx, active + 1);
    mark("drop");
    return true;
}

// the place of the drop for this dead enemy: its body, or where it stood when the body is out of reach
static bool place_for(const ps::Elite& e, ps::Vec3* out, bool* on_body) {
    ps::Vec3 body = e.rest; float y = 0.0f;
    bool body_ok = e.rest_ok && ground_y(body, &y, e.root);
    if (body_ok) body.y = y;
    bool clear = false;
    if (body_ok) { ps::Vec3 a = e.last_alive, b = body; a.y += 0.6f; b.y += 0.6f; clear = !blocked(a, b, e.root); }
    ps::Vec3 p = ps::drop_spot(g_c, e.last_alive, body, body_ok, clear, on_body);
    if (!*on_body) { if (ground_y(p, &y, e.root)) p.y = y; else return false; }
    // a body lies against a wall as often as not: the pickup comes away from it, half in a wall it looks wrong
    static const float DIRS[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    ps::Vec3 c = p; c.y += 0.4f; float px = 0.0f, pz = 0.0f;
    for (const float* d : DIRS) { ps::Vec3 t = c; t.x += d[0] * 0.5f; t.z += d[1] * 0.5f; if (blocked(c, t, e.root)) { px -= d[0] * 0.4f; pz -= d[1] * 0.4f; } }
    if (px != 0.0f || pz != 0.0f) {
        ps::Vec3 t = c; t.x += px; t.z += pz;
        if (!blocked(c, t, e.root) && ground_y(t, &y, e.root) && fabsf(y - p.y) < 0.5f) { p.x = t.x; p.z = t.z; p.y = y; }
    }
    // the floor under the pickup's footprint: the highest of the centre and four points 0.25 m around it (a slope or a stair
    // edge never swallows half of it); a point more than 0.35 m higher is a wall or a ledge (the same rule as [drops])
    {
        static const float FP[4][2] = {{0.25f, 0}, {-0.25f, 0}, {0, 0.25f}, {0, -0.25f}};
        float top = p.y;
        for (const float* d : FP) { ps::Vec3 q = p; q.x += d[0]; q.z += d[1]; if (ground_y(q, &y, e.root) && y > top && y <= p.y + 0.35f) top = y; }
        p.y = top;
    }
    p.y += g_drop_lift;
    *out = p; return true;
}

static void book_update(DWORD ram, const ps::World& w) {
    static ps::Cand cand[48];
    int n = w.ram_ok ? scan(ram, w.ram, cand, 48, g_c.drop_track_radius) : 0;
    for (int i = 0; i < n; ++i) {
        const ps::Cand& c = cand[i];
        if (!ps::drop_eligible(g_c, c)) continue;
        uint32_t key = c.ph ? c.ph : c.root;
        ps::Elite* x = g_book.find(key);
        if (!x) {
            x = g_book.add(key, c.uid, ps::ability_for(c.uid, g_c.moves));
            if (!x) continue;
            if (g_verbose) gp_log("psycho: elite %08X (placement %06X, hp %.0f): its ability is %s\n", c.root, c.uid, c.health, ps::move_name(x->ability));
        }
        x->saw(c);
    }
}

static void book_watch(const ps::World& w, int pm, bool after_gap) {
    for (ps::Elite& e : g_book.e) {
        if (!e.used || !e.alive_seen || e.dead) continue;
        float hp = 0.0f, dmg = 0.0f; int st = -1; bool same = false;
        __try {
            same = sub(e.root, R_AI) == e.ai && placeholder_of(e.root) == e.ph;
            DWORD a = data(e.ai); if (same && a) { hp = RDF(a + 0x2C); dmg = RDF(a + 0x30); st = (int)RD32(a + 0x1D4); }
        } __except (EXCEPTION_EXECUTE_HANDLER) { same = false; }
        int life = ps::life_of(same, hp, dmg, st);
        if (life == ps::LIFE_GONE) { e.alive_seen = false; continue; }   // the shell went to another placement: nothing died
        if (life == ps::LIFE_ALIVE) { ps::Vec3 p; if (get_pos(e.root, &p)) e.last_alive = p; continue; }
        e.dead = true; e.t_dead = w.t; e.rest_ok = false; e.next_try = 0;
        e.no_drop = gp_cutscene() || pm == 3 || after_gap;
        Who who; e.body = fill_who(e.root, &who) && who.anim ? find_path(who.anim, "Bip/Pelvis") : 0;
        gp_log("psycho: elite %08X (placement %06X) died (hp %.0f/%.0f, state %d), its ability %s: %s\n", e.root, e.uid, hp - dmg, hp, st, ps::move_name(e.ability),
               e.dropped ? "this placement already dropped once" : e.no_drop ? "no drop (cut-scene / rewind)" : !g_c.drops ? "drops off" : "drop follows");
    }
}

static void bodies_tick(const ps::World& w, int pm) {
    bool can = g_c.drops && w.psycho && pm != 2 && pm != 3 && !gp_cutscene();
    for (ps::Elite& e : g_book.e) {
        if (!ps::wants_drop(e)) continue;
        if (sub(e.root, R_AI) == e.ai && placeholder_of(e.root) == e.ph) {   // not reused yet: follow the body (ragdoll)
            ps::Vec3 p;
            if (get_pos(e.body ? e.body : e.root, &p)) {
                ps::Vec3 m = ps::sub(p, e.rest);
                if (!e.rest_ok || m.x * m.x + m.y * m.y + m.z * m.z > 0.0004f) { e.t_rest = w.t; e.rest = p; e.rest_ok = true; }
            }
        }
        if (!can || !ps::body_settled(g_c, e, w.t) || w.t < e.next_try) continue;
        if (w.t > e.t_dead + g_c.drop_give_up) {
            e.no_drop = true; ++g_drops_failed;
            gp_log("psycho: drop of %08X given up after %.0f s: no goodie prop came free\n", e.root, g_c.drop_give_up);
            continue;
        }
        ps::Vec3 at; bool on_body = false;
        if (!place_for(e, &at, &on_body)) {
            e.no_drop = true; ++g_drops_failed;
            gp_log("psycho: drop of %08X: no ground under the body or where it stood: no drop\n", e.root);
            continue;
        }
        if (make_drop(&e, e.ability, at, w.t, w.ram, w.ram_ok)) {
            e.dropped = true; e.t_drop = w.t;
            if (g_verbose) gp_log("psycho: ... %s, %.1f m from where it stood, %.2f s after the death\n", on_body ? "on the body" : "where it stood (body out of reach)", ps::dist_xz(at, e.last_alive), w.t - e.t_dead);
        } else {
            e.next_try = w.t + 0.5;
            if (g_verbose) gp_log("psycho: drop of %08X waits: no goodie prop free or %d drops on the ground\n", e.root, g_c.drop_max);
        }
    }
}

static void trace(const Drop& d, const ps::World& w) {   // test copy: every change of the prop, as the game sees it
    static DWORD lasts[5][8]; DWORD now[8] = {0}; DWORD* last = lasts[&d - g_drops];
    __try {
        DWORD pd = data(d.prop), ad = data(d.ai), ctrl = RD32(pd + P_CTRL), cd = data(ctrl); ps::Vec3 p = {0, 0, 0}; get_pos(d.prop, &p);
        now[0] = RD32(pd + P_STATUS); now[1] = RD32(pd + P_PLACEHOLDER); now[2] = RD32(d.prop + 0x28) & 0xFFF; now[3] = (DWORD)(int)(p.x * 10) ^ ((DWORD)(int)(p.y * 10) << 10) ^ ((DWORD)(int)(p.z * 10) << 20);
        now[4] = cd ? RD32(cd + 0x28) : 0; now[5] = cd ? RD32(cd + 0x2C) : 0; now[6] = RD32(pd + P_SUBTYPE); now[7] = RD32(ad + PA_TIMER);
        if (memcmp(now, last, sizeof now)) {
            memcpy(last, now, sizeof now);
            gp_log("psycho: trace prop %08X: status %d ph %08X flags %03X pos (%.2f %.2f %.2f) owner %08X success %d sub %d timer %.1f, Ram %.1f m, ai %s\n", d.prop, now[0], now[1], now[2], p.x, p.y, p.z,
                   now[4], now[5], now[6], RDF(ad + PA_TIMER), ps::dist_xz(p, w.ram), "");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void drops_watch(const ps::World& w) {
    for (Drop& d : g_drops) {
        if (!d.used) continue;
        if (d.taken) { if (w.t > d.t_taken + 15.0) d.used = false; continue; }
        if (g_debug_trace) trace(d, w);
        int st = 0; DWORD ph = 0;
        if (!prop_state(d.prop, &st, &ph)) { d.used = false; continue; }
        if (st == ST_DROP && ph == 0 && w.t < d.t_end) {   // a charge spent meanwhile: the pickup is still one more than this
            int c = loco_count(d.slot, nullptr); if (c >= 0 && c < d.count0) d.count0 = c;
            continue;
        }
        ps::Elite* e = g_book.find(d.key);
        if (st == ST_DROP && ph == 0) { // its time is up (the prop's own timer starts over when a full slot refuses it)
            free_prop(d.prop); ++g_drops_gone;
            gp_log("psycho: DROP %s (prop %08X) stayed %.0f s: gone\n", ps::move_name(d.ability), d.prop, g_c.drop_seconds);
        } else {
            int c = loco_count(d.slot, nullptr);
            if (c > d.count0) {
                ++g_drops_taken; if (e) e->collected = true;
                d.taken = true; d.t_taken = w.t;
                if (g_debug_ram_dies > 0.0f && g_debug_ram_dies_on == 1 && g_debug_die_t < 0.0) g_debug_die_t = w.t + g_debug_ram_dies;
                gp_log("psycho: DROP %s COLLECTED after %.1f s: Ram holds %d (was %d)\n", ps::move_name(d.ability), w.t - d.t0, c, d.count0);
                mark("collected");
                continue;
            } else {
                ++g_drops_gone;
                gp_log("psycho: DROP %s gone after %.1f s of %.0f (its own timer, or the game took the prop; Ram holds %d)\n", ps::move_name(d.ability), w.t - d.t0, g_c.drop_seconds, c);
            }
        }
        d.used = false;
    }
}

// the clock went back. More than a rewind can do = a load or retry: everything starts over (the sweep frees props a save
// brought back). Otherwise a rewind: every death after the clock's new time never happened, its drop goes; a drop made after
// the new time but whose enemy stays dead is made again (the rewind may have put the prop's members back). A rewind past a
// pickup puts Ram's count and the prop back (seen in game): the drop lies there again and is watched again.
static void clock_back(double t, double back) {
    if (ps::clock_jump_is_load(back)) {
        gp_log("psycho: the clock jumped %.0f s (load / retry): %d tracked elites and the drops forgotten\n", back, g_book.count());
        g_book.clear(); for (Drop& d : g_drops) d.used = false;
        return;
    }
    for (Drop& d : g_drops) {
        if (!d.used || !d.taken || t >= d.t_taken) continue;
        ps::Elite* e = g_book.find(d.key);
        if (e) e->collected = false;
        int st = 0; DWORD ph = 0;
        if (prop_state(d.prop, &st, &ph) && st == ST_DROP && !ph) {
            d.taken = false; d.count0 = loco_count(d.slot, nullptr);
            gp_log("psycho: rewind before the pickup of %08X: the drop lies there again (Ram holds %d)\n", d.key, d.count0);
            for (Drop& x : g_drops) if (&x != &d && x.used && !x.taken && x.prop == d.prop) {   // a later drop had the same prop
                x.used = false; ps::Elite* o = g_book.find(x.key); if (o) { o->dropped = false; o->next_try = 0; }
            }
        } else {
            d.used = false;
            if (e) { e->dropped = false; e->next_try = 0; }
            gp_log("psycho: rewind before the pickup of %08X: made again\n", d.key);
        }
    }
    for (ps::Elite& e : g_book.e) {
        if (!e.used || !e.dead || t >= e.t_dead) continue;
        bool free_it = ps::undo_death(e, t);
        ++g_drops_undone;
        gp_log("psycho: rewind before the death of %08X: it lives again%s\n", e.root, free_it ? ", its drop goes" : e.collected ? " (its drop stays collected)" : "");
        for (Drop& d : g_drops) if (d.used && d.key == e.key && d.t0 >= e.t_dead) { if (!d.taken) free_prop(d.prop); d.used = false; }   // an earlier life's drop stays
    }
    for (Drop& d : g_drops) {
        if (!d.used || d.taken || t >= d.t0) continue;
        ps::Elite* e = g_book.find(d.key);
        free_prop(d.prop); d.used = false;
        if (e) { e->dropped = false; e->next_try = 0; }
        gp_log("psycho: rewind before the drop of %08X: made again\n", d.key);
    }
}

// a prop in status 2 that is not one of ours: a save brought it back (saves store the pool). It goes.
static void sweep(double t) {
    if (t < g_drop_sweep_t) return;
    g_drop_sweep_t = t + 1.0;
    DWORD props[16]; int n = pool_props(props, 16);
    for (int i = 0; i < n; ++i) {
        int st = 0; DWORD ph = 0;
        if (!prop_state(props[i], &st, &ph) || st != ST_DROP || ph) continue;
        bool mine = false; for (Drop& d : g_drops) if (d.used && !d.taken && d.prop == props[i]) mine = true;
        if (mine) continue;
        free_prop(props[i]);
        gp_log("psycho: goodie prop %08X was a drop of an earlier session (a save): given back to the pool\n", props[i]);
    }
}

static void debug_drop(DWORD ram, const ps::World& w) {
    if (!InterlockedExchange(&g_debug_drop, 0) || !w.ram_ok) return;
    ps::Vec3 f = {0, 0, 1}; get_fwd(ram, &f);
    ps::Vec3 at = w.ram; float y = at.y; bool g = false;
    for (int i = 0; i < 8 && !g; ++i) {        // the first of eight directions with a clear line and ground
        float a = i * 0.7853982f, sa = sinf(a), ca = cosf(a);
        ps::Vec3 dir = ps::v3(f.x * ca + f.z * sa, 0, f.z * ca - f.x * sa), t = ps::add(w.ram, ps::mul(dir, 3.0f)), r1 = w.ram, r2 = t;
        r1.y += 0.6f; r2.y += 0.6f;
        if (blocked(r1, r2, ram) || !ground_y(t, &y, ram)) continue;
        at = t; g = true;
    }
    at.y = g ? y + g_drop_lift : at.y;
    int move = g_debug_move == 1 || g_debug_move == 2 ? g_debug_move : ps::MOVE_TORNADO;
    DWORD props[16]; int n = pool_props(props, 16), fr = 0;
    for (int i = 0; i < n; ++i) { int st = 0; DWORD ph = 0; if (prop_state(props[i], &st, &ph) && st == 0) ++fr; }
    for (int i = 0; i < n; ++i) {
        __try {
            int st = 0; DWORD ph = 0; prop_state(props[i], &st, &ph); DWORD m = RD32(props[i] + 0x30); ps::Vec3 p = {0, 0, 0}; get_pos(props[i], &p);
            DWORD pc = RD32(data(props[i]) + P_CTRL), mc = RD32(pc + 0x30);
            gp_log("psycho:   pool prop %08X status %d ph %08X flags %03X sub %d pos (%.1f %.1f %.1f) node+50 %.2f model %08X +3C %d +3D %d +3E %d +54 %u | ctrl node+50 %.2f model %08X +3C %d +3D %d +3E %d | ph+104 %.2f\n",
                   props[i], st, ph, RD32(props[i] + 0x28) & 0xFFF, RD32(data(props[i]) + P_SUBTYPE), p.x, p.y, p.z, RDF(props[i] + 0x50), m, m ? *(BYTE*)(m + 0x3C) : 0, m ? *(BYTE*)(m + 0x3D) : 0, m ? *(unsigned short*)(m + 0x3E) : 0, m ? RD32(m + 0x54) : 0,
                   RDF(pc + 0x50), mc, mc ? *(BYTE*)(mc + 0x3C) : 0, mc ? *(BYTE*)(mc + 0x3D) : 0, mc ? *(unsigned short*)(mc + 0x3E) : 0, ph ? RDF(ph + 0x104) : 0.0f);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    gp_log("psycho: debug_drop_key: %s 3 m ahead, ground %s (Ram y %.2f, ground %.2f); goodie pool %d props, %d free\n", ps::move_name(move), g ? "found" : "NOT found", w.ram.y, y, n, fr);
    if (!make_drop(nullptr, move, at, w.t, w.ram, w.ram_ok)) gp_log("psycho: debug_drop_key: no prop\n");
}
static void debug_walk(DWORD ram, const ps::World& w) {
    if (!g_debug_drop_walk) return;
    Drop* best = nullptr; float bd = 1e9f;
    for (Drop& d : g_drops) {
        int mx = 0, c = d.used ? loco_count(d.slot, &mx) : 0;
        if (!d.used || c >= mx) continue;      // a full slot refuses it
        float x = ps::dist_xz(d.pos, w.ram); if (x < bd) { bd = x; best = &d; }
    }
    if (!best) return;
    ps::Vec3 f; if (!get_fwd(ram, &f)) return;
    float a = ps::yaw_of(ps::norm_xz(ps::sub(best->pos, w.ram))) - ps::yaw_of(f);
    while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f;
    if (fabsf(a) > 0.02f) gp_yaw_add(a * 57.29578f * 0.25f);
    if (w.t - best->t0 >= g_debug_walk_wait) g_debug_walk_frames = 3;   // first it faces the drop: the proof clip shows it lying there
}

// every tick, before anything else of the feature (it must see deaths during a move, in a vehicle, ...)
static void drops_tick(DWORD ram, const ps::World& w) {
    int pm = gp_playmode();
    if (g_debug_rewinds > 0 && w.ram_ok) {     // test copy: rewind charges for the rewind proof (TAB needs one)
        __try {
            int mx = 0, c = loco_count(1, &mx);
            if (c == 0 && mx > 0) {
                DWORD pd = data(sub(ram, R_POWERUP)), p = RD32(pd + 0x14), L0 = RD32(p), k = list_stride(A_LIST_INT);
                WR32(RD32(RD32(L0 + 4) + 4) + 1 * 4 * k, g_debug_rewinds < mx ? g_debug_rewinds : mx);
                gp_log("psycho: debug_rewinds: Ram has %d rewind charge(s) of %d\n", loco_count(1, nullptr), mx);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (g_debug_die_t >= 0.0 && !g_debug_died && w.ram_ok && w.t >= g_debug_die_t) {   // test copy: a rewind starts at Ram's death prompt
        g_debug_died = true;
        struct Hit { float dmg; DWORD owner, inflictor, collvol; float imp[3]; float pos[3]; int type; };   // take_a_hit, mh_game.cpp
        __try {
            Hit h = {100000.0f, ram, ram, 0, {0.0f, 0.0f, 0.0f}, {w.ram.x, w.ram.y + 1.0f, w.ram.z}, 1};
            command(ram, 215, &h);
            gp_log("psycho: debug_ram_dies: a hit of 100000 to Ram (rewind charges %d, clock %.2f)\n", loco_count(1, nullptr), w.t);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (g_debug_trace) {                       // test copy: Ram's counts and the clock around a rewind
        static int c0 = -2, c1 = -2, c2 = -2, pm0 = -2;
        int a = loco_count(3, nullptr), b = loco_count(4, nullptr), r = loco_count(1, nullptr);
        if (a != c0 || b != c1 || r != c2 || pm != pm0) { gp_log("psycho: trace Ram holds tornado %d, toro %d, rewinds %d; playmode %d, clock %.2f\n", a, b, r, pm, w.t); c0 = a; c1 = b; c2 = r; pm0 = pm; }
    }
    // the tick runs while the player is in control: game time that passed without it was a cut-scene, and a death seen on the
    // first tick after it happened in the cut-scene
    bool after_gap = g_drop_clock >= 0.0 && w.t > g_drop_clock + 1.0;
    if (g_drop_clock >= 0.0 && w.t < g_drop_clock - 0.001) {
        double back = g_drop_clock - w.t;
        if (back > 0.05 || pm == 3) g_drop_back = true;   // the tick does not run while a rewind plays
        if (ps::clock_jump_is_load(back)) { clock_back(w.t, back); g_drop_back = false; }
    }
    g_drop_clock = w.t;
    if (pm == 3) return;                       // the rewind plays: everything is looked at when it is over
    if (g_drop_back) { g_drop_back = false; clock_back(w.t, 0.0); }
    if (pm == 2) return;
    if (!w.psycho) { drops_watch(w); return; }
    if (w.t >= g_drop_scan_t) { g_drop_scan_t = w.t + 0.5; book_update(ram, w); }
    book_watch(w, pm, after_gap);
    bodies_tick(w, pm);
    drops_watch(w);
    if (g_c.drops) { sweep(w.t); debug_drop(ram, w); if (w.ram_ok) debug_walk(ram, w); }
}

static void drops_level_change() {             // a new level / mission: nothing carries over (the sweep frees props left on the ground)
    int n = 0; for (Drop& d : g_drops) { if (d.used && !d.taken) ++n; d.used = false; }
    if (n || g_book.count()) gp_log("psycho: level / mission changed: %d tracked elites, %d drops forgotten\n", g_book.count(), n);
    g_book.clear(); g_drop_back = false; g_drop_scan_t = 0.0; g_drop_sweep_t = 0.0; g_drop_clock = -1.0;
}
static void drops_fill_abilities(ps::Cand* cand, int n) {
    for (int i = 0; i < n; ++i) { ps::Elite* x = g_book.find(cand[i].ph ? cand[i].ph : cand[i].root); if (x) cand[i].ability = x->ability; }
}
static void drops_post_input(BYTE* state) {
    static bool held = false;
    if (g_debug_drop_key >= 0 && g_debug_drop_key < 256) {
        bool h = (state[g_debug_drop_key] & 0x80) != 0;
        if (h && !held) InterlockedExchange(&g_debug_drop, 1);
        held = h;
    }
    if (g_debug_walk_frames > 0) { __try { state[0x11 /*W*/] = 0x80; } __except (EXCEPTION_EXECUTE_HANDLER) {} --g_debug_walk_frames; }
}
