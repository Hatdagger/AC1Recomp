/* Master of Arena system menu: the AC builder (the Raven Card Builder in the
 * launcher). Included by moa_menu_ai.inc; works on the same card being edited
 * (g_edit) as the AI editor.
 *
 *   BUILD     names, the eleven part slots, optional parts, paint, emblem,
 *             Human PLUS, the card's AI, save. Stats and the game's checks
 *             on the right, as the web builder shows them.
 *   PICK      the full list for one part slot, grouped as the web builder does.
 *   OPTIONS   the twelve optional parts against the core's slots.
 *   PAINT     four sections x base / optional / detail / joint, RGB 0..64.
 *   EMBLEM    a picture from the Emblems folder (or any file), cut to the
 *             game's 64x64, 16 colours.
 *
 * Part names and numbers come from the player's own disc (moa_parts.c).
 */

#include "../../../../../games/master-of-arena/moa_parts.h"

static moa_parts g_parts;
static int g_parts_state = 0;           /* 0 not read, 1 ok, -1 failed */
static bool g_paint_auto = false;        /* paint follows the emblem until edited */
static bool g_split_paint = false;       /* edit the four sections separately */
static int g_name_edit = 0;              /* 1 AC name, 2 pilot name */
static int g_name_pos = 0;
static int g_pick_byte = -1;             /* record byte being picked */
static int g_paint_section = 0;
static std::string g_emblem_pending;     /* picked by the file dialog */
static std::vector<std::string> g_emblem_files;

static const int kNameMax = 7;           /* builder: NAME_MAX */
static const char kNameChars[] =
    " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.,!?/&*#@:;'\"()[]+=<$%~|"
    "abcdefghijklmnopqrstuvwxyz";

/* The builder's slots: label, record byte, may be empty. */
static const struct { const char* label; int byte; bool optional; } kSlots[] = {
    { "HEAD", 1, false }, { "CORE", 0, false }, { "ARMS", 2, false }, { "LEGS", 3, false },
    { "BOOSTER", 12, true }, { "FCS", 8, false }, { "GENERATOR", 9, false },
    { "RIGHT ARM", 4, false }, { "LEFT ARM", 5, true }, { "BACK LEFT", 6, true },
    { "BACK RIGHT", 7, true },
};
static const int kSlotCount = (int)(sizeof kSlots / sizeof kSlots[0]);

static bool paint_uniform();

static bool parts_ready(const LauncherModel* m) {
    if (g_parts_state == 0) {
        const char* p = launcher_model_effective_rom_path(m);
        if (!p || !p[0]) p = launcher_model_rom_path(m);
        g_parts_state = (p && p[0] && moa_parts_load(p, &g_parts)) ? 1 : -1;
    }
    return g_parts_state > 0;
}

static bool slot_locked(int byte) {
    const uint8_t* r = g_edit.rec;
    if ((byte == 4 || byte == 5) && r[2] >= 12 && r[2] != 255) return true;   /* weapon arms */
    if (byte == 12 && r[3] >= 27 && r[3] != 255) return true;                 /* caterpillar */
    return false;
}

static std::string part_name(int byte) {
    const uint8_t id = g_edit.rec[byte];
    if (id == 255) return "NONE";
    const moa_part* p = moa_parts_find(&g_parts, moa_record_cat(byte), id);
    return p ? std::string(p->name) : std::string("?");
}

/* The parts a slot can take, in table order, NONE first when it may be empty. */
static std::vector<int> slot_choices(int byte, bool optional) {
    std::vector<int> v;
    if (optional) v.push_back(255);
    const int cat = moa_record_cat(byte);
    for (int i = 0; i < g_parts.count[cat]; ++i) v.push_back(g_parts.part[cat][i].id);
    return v;
}

static void edit_changed() {
    moa_build_normalise(g_edit.rec);
    g_edit_dirty = true;
}

static void step_part(int byte, bool optional, int dir) {
    if (slot_locked(byte)) return;
    const std::vector<int> v = slot_choices(byte, optional);
    if (v.empty()) return;
    int at = 0;
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == g_edit.rec[byte]) at = (int)i;
    at = (at + dir + (int)v.size()) % (int)v.size();
    g_edit.rec[byte] = (uint8_t)v[(size_t)at];
    edit_changed();
}

static std::string plus_text(uint8_t plus) {
    if (plus == 0) return "OFF";
    if (plus >= 9) return "FULL";
    char b[16];
    std::snprintf(b, sizeof b, "LEVEL %d", plus);
    return b;
}

/* ---- right-hand panels ---- */

static void part_stats(const Gfx& g, float x, float y, int cat, const moa_part* p) {
    if (!p) { text(g, x, y, 11.0f, kDim, "NONE"); return; }
    text(g, x, y, 13.0f, kBright, p->name);
    const char* grp = moa_part_group(cat, p->name);
    if (grp[0]) text(g, x, y + 18.0f, 10.0f, kDim, grp);
    char b[64];
    float yy = y + 36.0f;
    auto line = [&](const char* label, int v) {
        text(g, x, yy, 10.0f, kDim, label);
        std::snprintf(b, sizeof b, "%d", v);
        text_right(g, x + 240.0f, yy, 10.0f, kText, b);
        yy += 14.0f;
    };
    line("WEIGHT", p->weight);
    if (cat == MOA_CAT_GEN) line("OUTPUT", p->extra); else line("ENERGY DRAIN", p->drain);
    if (p->ap) line("AP", p->ap);
    if (p->def_shell || p->def_energy) { line("DEF. SHELL", p->def_shell); line("DEF. ENERGY", p->def_energy); }
    if (cat == MOA_CAT_CORE) { line("ARM LOAD", p->extra); line("OPTION SLOTS", p->slots); }
    if (cat == MOA_CAT_LEGS) line("LOAD", p->extra);
    if (cat == MOA_CAT_BOOST) line("POWER", p->extra);
}

static void meter_line(const Gfx& g, float x, float y, const char* label, int v, int max,
                       bool over, bool waived) {
    char b[48];
    std::snprintf(b, sizeof b, "%d / %d", v, max);
    text(g, x, y, 10.0f, kDim, label);
    text_right(g, x + 240.0f, y, 10.0f, over ? (waived ? kHi : kWarn) : kText, b);
}

static void build_totals(const Gfx& g, float x, float y) {
    struct moa_build_check t;
    moa_build_check(&g_parts, g_edit.rec, g_edit.plus, &t);
    char b[48];
    text(g, x, y, 10.0f, kDim, "WEIGHT");
    std::snprintf(b, sizeof b, "%d", t.weight); text_right(g, x + 240.0f, y, 10.0f, kText, b);
    text(g, x, y + 14.0f, 10.0f, kDim, "AP");
    std::snprintf(b, sizeof b, "%d", t.ap); text_right(g, x + 240.0f, y + 14.0f, 10.0f, kText, b);
    text(g, x, y + 28.0f, 10.0f, kDim, "DEF. SHELL / ENERGY");
    std::snprintf(b, sizeof b, "%d / %d", t.def_shell, t.def_energy);
    text_right(g, x + 240.0f, y + 28.0f, 10.0f, kText, b);
    const bool plus = g_edit.plus > 0;
    meter_line(g, x, y + 46.0f, "LEGS LOAD", t.leg_load, t.leg_max, t.leg_over, plus);
    meter_line(g, x, y + 60.0f, "ARM LOAD", t.arm_load, t.arm_max, t.arm_over, plus);
    meter_line(g, x, y + 74.0f, "ENERGY", t.drain, t.output, t.energy_over, plus);
    meter_line(g, x, y + 88.0f, "OPTION SLOTS", t.slots_used, t.slots_max, t.slots_over, false);
    const char* verdict = "THIS AC CAN BE FIELDED.";
    ImU32 col = kOn;
    if (t.missing_booster) { verdict = "THESE LEGS NEED A BOOSTER."; col = kWarn; }
    else if (t.slots_over) { verdict = "THE OPTIONAL PARTS NEED MORE SLOTS THAN THIS CORE HAS."; col = kWarn; }
    else if (t.blocked) { verdict = "OVER THE LIMITS. THE GAME WILL NOT FIELD THIS AC WITHOUT HUMAN PLUS."; col = kWarn; }
    else if (t.leg_over || t.arm_over || t.energy_over) { verdict = "OVER THE LIMITS, ALLOWED WITH HUMAN PLUS."; col = kHi; }
    text(g, x, y + 108.0f, 10.0f, col, verdict, 240.0f);
}

static bool build_blocked() {
    struct moa_build_check t;
    moa_build_check(&g_parts, g_edit.rec, g_edit.plus, &t);
    return t.blocked != 0;
}

/* ---- names ---- */

static char* name_buf(int which) { return which == 1 ? g_edit.ac : g_edit.pilot; }

static void name_input(const Input& in) {
    char* s = name_buf(g_name_edit);
    int len = (int)std::strlen(s);
    if (len > kNameMax) { s[kNameMax] = 0; len = kNameMax; }
    auto put = [&](char ch) {
        if (g_name_pos >= kNameMax) return;
        if (g_name_pos >= len) { while (len < g_name_pos) s[len++] = ' '; s[len++] = ch; s[len] = 0; }
        else s[g_name_pos] = ch;
        if (g_name_pos < kNameMax - 1) g_name_pos++;
        g_edit_dirty = true;
    };
    /* Keyboard typing. */
    ImGuiIO& io = ImGui::GetIO();
    bool typed_space = false;
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        const ImWchar c = io.InputQueueCharacters[i];
        if (c < 128 && std::strchr(kNameChars, (int)c) && c != 0) { put((char)c); if (c == ' ') typed_space = true; }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) {
        if (g_name_pos > 0 && (g_name_pos >= len || s[g_name_pos] == ' ')) g_name_pos--;
        if (g_name_pos < len) { std::memmove(s + g_name_pos, s + g_name_pos + 1, (size_t)(len - g_name_pos)); }
        g_edit_dirty = true;
        return;
    }
    if (in.left && g_name_pos > 0) g_name_pos--;
    if (in.right && g_name_pos < kNameMax - 1 && g_name_pos < len) g_name_pos++;
    if (in.up || in.down) {
        const char cur = g_name_pos < len ? s[g_name_pos] : ' ';
        const char* at = std::strchr(kNameChars, cur);
        const int n = (int)std::strlen(kNameChars);
        int k = at ? (int)(at - kNameChars) : 0;
        k = (k + (in.down ? 1 : n - 1)) % n;
        const int keep = g_name_pos;
        put(kNameChars[k]);
        g_name_pos = keep;
    }
    if (in.alt && g_name_pos < len) {           /* X: delete this character */
        std::memmove(s + g_name_pos, s + g_name_pos + 1, (size_t)(len - g_name_pos));
        g_edit_dirty = true;
    }
    const bool done = (in.confirm && !typed_space) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                      ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                      ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false);
    if (done) {
        /* trim trailing spaces */
        int l = (int)std::strlen(s);
        while (l > 0 && s[l - 1] == ' ') s[--l] = 0;
        g_name_edit = 0;
    }
}

static std::string name_shown(int which) {
    std::string s = name_buf(which);
    if (g_name_edit != which) return s.empty() ? std::string("-") : s;
    if ((int)s.size() <= g_name_pos) s.resize((size_t)g_name_pos + 1, ' ');
    const bool blink = std::fmod(ImGui::GetTime(), 0.8) < 0.5;
    if (blink) s[(size_t)g_name_pos] = '_';
    return s;
}

/* ---- BUILD ---- */

enum { B_AC = 0, B_PILOT, B_SLOT0, B_OPTS = B_SLOT0 + 11, B_PAINT, B_EMBLEM, B_PLUS, B_AI,
       B_SAVE, B_SAVE_NEW, B_ROWS };

static void open_build_new() {
    moa_card_new(&g_edit);
    g_edit_path.clear();
    g_edit_dirty = true;
    g_paint_auto = true;
    g_split_paint = false;
    moa_card_paint_from_emblem(&g_edit);
    g_confirm_discard = false;
    g_name_edit = 0;
    g_ai_sub = AI_SUB_BUILD;
    g_state.cursor[PAGE_AI_BUILD] = 0;
    g_state.top[PAGE_AI_BUILD] = 0;
}

static void open_build(const AiCard& c) {
    g_edit = c.card;
    if (!g_edit.has_ai) moa_card_set_preset(&g_edit, MOA_AI_PRESET_BALANCED);
    g_edit_path = c.path;
    g_edit_dirty = !c.card.has_ai;
    g_paint_auto = false;
    g_split_paint = false;
    g_confirm_discard = false;
    g_name_edit = 0;
    g_ai_sub = AI_SUB_BUILD;
    g_state.cursor[PAGE_AI_BUILD] = 0;
    g_state.top[PAGE_AI_BUILD] = 0;
}

static bool save_build(LauncherModel* m, bool as_new) {
    if (build_blocked()) { note("THE GAME WILL NOT FIELD THIS AC: FIX THE BUILD FIRST"); return false; }
    if (!g_edit.ac[0]) std::strcpy(g_edit.ac, "AC");
    return save_edit(m, as_new || g_edit_path.empty());
}

static void draw_ai_build(LauncherModel* m, const Gfx& g, const Input& in) {
    header(g, "AC BUILDER", g_edit_path.empty() ? "NEW CARD" : upper(g_edit.ac).c_str());
    if (!parts_ready(m)) {
        panel(g, 24.0f, 76.0f, 592.0f, 120.0f, "PARTS");
        text(g, 38.0f, 96.0f, 12.0f, kWarn, "THE PART LIST COULD NOT BE READ FROM THE GAME DISC.");
        text(g, 38.0f, 120.0f, 10.0f, kText,
             "The builder reads the garage's part tables from your Master of Arena disc 1. "
             "Check that the disc image is set in Settings (or sits beside the program).", 560.0f);
        footer(g, "B  BACK");
        if (in.back) { g_parts_state = 0; g_ai_sub = AI_SUB_CARDS; }
        return;
    }
    const int visible = 14;
    if (!g_name_edit && !g_confirm_discard) move_cursor(PAGE_AI_BUILD, B_ROWS, in, visible);
    int& cur = g_state.cursor[PAGE_AI_BUILD];
    const int top = g_state.top[PAGE_AI_BUILD];
    struct moa_build_check chk;
    moa_build_check(&g_parts, g_edit.rec, g_edit.plus, &chk);

    panel(g, 24.0f, 62.0f, 300.0f, visible * kRowH + 12.0f, g_edit_dirty ? "BUILD  (NOT SAVED)" : "BUILD");
    int activate = -1;
    for (int i = top; i < B_ROWS && i < top + visible; ++i) {
        std::string label, value;
        bool enabled = true;
        if (i == B_AC) { label = "AC NAME"; value = name_shown(1); }
        else if (i == B_PILOT) { label = "PILOT NAME"; value = name_shown(2); }
        else if (i < B_OPTS) {
            const int k = i - B_SLOT0;
            label = kSlots[k].label;
            enabled = !slot_locked(kSlots[k].byte);
            value = enabled ? part_name(kSlots[k].byte) : "-";
        } else if (i == B_OPTS) {
            label = "OPTIONAL PARTS";
            char b[32]; std::snprintf(b, sizeof b, "%d / %d SLOTS", chk.slots_used, chk.slots_max);
            value = b;
        } else if (i == B_PAINT) { label = "PAINT"; value = g_paint_auto ? "FROM EMBLEM" : "CUSTOM"; }
        else if (i == B_EMBLEM) { label = "EMBLEM"; value = "CHANGE"; }
        else if (i == B_PLUS) { label = "HUMAN PLUS"; value = plus_text(g_edit.plus); }
        else if (i == B_AI) { label = "AI"; value = card_ai_label(g_edit); }
        else if (i == B_SAVE) { label = "SAVE CARD"; enabled = !chk.blocked; }
        else { label = "SAVE AS NEW CARD"; enabled = !chk.blocked; }
        bool hovered = false;
        if (row(g, 25.0f, 74.0f + kRowH * (i - top), 298.0f, i == cur, enabled, label.c_str(),
                value.c_str(), false, false, &hovered) && !g_name_edit && !g_confirm_discard)
            activate = i;
        if (hovered && !g_name_edit && (ImGui::GetIO().MouseDelta.x != 0.0f ||
                                        ImGui::GetIO().MouseDelta.y != 0.0f))
            cur = i;
    }

    panel(g, 340.0f, 62.0f, 276.0f, visible * kRowH + 12.0f, "AC");
    if (cur >= B_SLOT0 && cur < B_OPTS) {
        const int byte = kSlots[cur - B_SLOT0].byte;
        part_stats(g, 354.0f, 76.0f, moa_record_cat(byte),
                   g_edit.rec[byte] == 255 ? nullptr
                                           : moa_parts_find(&g_parts, moa_record_cat(byte), g_edit.rec[byte]));
    } else {
        draw_emblem(g, 354.0f, 76.0f, 72.0f, g_edit);
        text(g, 436.0f, 78.0f, 13.0f, kBright, upper(g_edit.ac).c_str());
        text(g, 436.0f, 98.0f, 10.0f, kDim, "PILOT");
        text(g, 436.0f, 112.0f, 11.0f, kText, upper(g_edit.pilot).c_str());
        text(g, 436.0f, 132.0f, 10.0f, kHi, ("AI  " + card_ai_label(g_edit)).c_str());
    }
    build_totals(g, 354.0f, 196.0f);
    if (g_name_edit)
        text(g, 354.0f, 340.0f, 10.0f, kHi,
             "UP / DOWN  LETTER   LEFT / RIGHT  MOVE   X  DELETE   A  DONE  (or type)", 250.0f);
    if (g_confirm_discard) {
        panel(g, 360.0f, 330.0f, 236.0f, 64.0f, "UNSAVED CHANGES");
        text(g, 374.0f, 346.0f, 11.0f, kWarn, "LEAVE WITHOUT SAVING?");
        text(g, 374.0f, 370.0f, 10.0f, kText, "A  LEAVE      B  STAY");
    }
    footer(g, "A  SELECT      LEFT / RIGHT  CHANGE      B  BACK");

    if (g_name_edit) { name_input(in); return; }
    if (g_confirm_discard) {
        if (in.back) { g_confirm_discard = false; return; }
        if (in.confirm) { g_confirm_discard = false; g_edit_dirty = false; g_ai_sub = AI_SUB_CARDS; }
        return;
    }
    if (in.back) {
        if (g_edit_dirty) g_confirm_discard = true; else g_ai_sub = AI_SUB_CARDS;
        return;
    }
    const int dir = in.left ? -1 : in.right ? 1 : 0;
    if (dir) {
        if (cur >= B_SLOT0 && cur < B_OPTS) step_part(kSlots[cur - B_SLOT0].byte, kSlots[cur - B_SLOT0].optional, dir);
        else if (cur == B_PLUS) {
            int s = moa_ai_rebel_step(g_edit.plus) + dir;
            s = s < 0 ? 4 : s > 4 ? 0 : s;
            g_edit.plus = moa_ai_rebel_level(s);
            g_edit_dirty = true;
        }
        return;
    }
    if (in.confirm) activate = cur;
    if (activate < 0) return;
    cur = activate;
    if (activate == B_AC || activate == B_PILOT) {
        g_name_edit = activate == B_AC ? 1 : 2;
        g_name_pos = 0;
    } else if (activate < B_OPTS) {
        const int k = activate - B_SLOT0;
        if (slot_locked(kSlots[k].byte)) return;
        g_pick_byte = k;
        const std::vector<int> v = slot_choices(kSlots[k].byte, kSlots[k].optional);
        g_state.cursor[PAGE_AI_PICK] = 0;
        for (size_t i = 0; i < v.size(); ++i) if (v[i] == g_edit.rec[kSlots[k].byte]) g_state.cursor[PAGE_AI_PICK] = (int)i;
        g_state.top[PAGE_AI_PICK] = 0;
        g_ai_sub = AI_SUB_PICK;
    } else if (activate == B_OPTS) { g_state.cursor[PAGE_AI_OPTS] = 0; g_ai_sub = AI_SUB_OPTS; }
    else if (activate == B_PAINT) { g_split_paint = !paint_uniform(); g_paint_section = 0;
                                    g_state.cursor[PAGE_AI_PAINT] = 0; g_state.top[PAGE_AI_PAINT] = 0; g_ai_sub = AI_SUB_PAINT; }
    else if (activate == B_EMBLEM) { g_state.cursor[PAGE_AI_EMBLEM] = 0; g_state.top[PAGE_AI_EMBLEM] = 0;
                                     g_emblem_files.clear(); g_ai_sub = AI_SUB_EMBLEM; }
    else if (activate == B_PLUS) {
        g_edit.plus = g_edit.plus ? 0 : 9;      /* builder: a toggle, full = 9 */
        g_edit_dirty = true;
    } else if (activate == B_AI) {
        g_edit_from_build = true;
        g_confirm_discard = false;
        g_state.cursor[PAGE_AI_EDIT] = 0;
        g_ai_sub = AI_SUB_EDIT;
    } else if (activate == B_SAVE) save_build(m, false);
    else if (activate == B_SAVE_NEW) save_build(m, true);
}

/* ---- PICK ---- */

static void draw_ai_pick(const Gfx& g, const Input& in) {
    const int k = g_pick_byte;
    if (k < 0 || k >= kSlotCount) { g_ai_sub = AI_SUB_BUILD; return; }
    const int byte = kSlots[k].byte, cat = moa_record_cat(byte);
    const std::vector<int> v = slot_choices(byte, kSlots[k].optional);
    const int visible = 14;
    move_cursor(PAGE_AI_PICK, (int)v.size(), in, visible);
    int& cur = g_state.cursor[PAGE_AI_PICK];
    const int top = g_state.top[PAGE_AI_PICK];
    header(g, kSlots[k].label, "CHOOSE A PART");
    panel(g, 24.0f, 62.0f, 300.0f, visible * kRowH + 12.0f, kSlots[k].label);
    int activate = -1;
    for (int i = top; i < (int)v.size() && i < top + visible; ++i) {
        const moa_part* p = v[(size_t)i] == 255 ? nullptr : moa_parts_find(&g_parts, cat, (uint8_t)v[(size_t)i]);
        std::string label = p ? p->name : "NONE";
        if (v[(size_t)i] == g_edit.rec[byte]) label += "  *";
        bool hovered = false;
        if (row(g, 25.0f, 74.0f + kRowH * (i - top), 298.0f, i == cur, true, label.c_str(),
                p ? moa_part_group(cat, p->name) : "", false, false, &hovered))
            activate = i;
        if (hovered && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f)) cur = i;
    }
    panel(g, 340.0f, 62.0f, 276.0f, visible * kRowH + 12.0f, "PART");
    if (cur >= 0 && cur < (int)v.size()) {
        const moa_part* p = v[(size_t)cur] == 255 ? nullptr : moa_parts_find(&g_parts, cat, (uint8_t)v[(size_t)cur]);
        part_stats(g, 354.0f, 76.0f, cat, p);
        /* the build with this part in */
        const uint8_t keep = g_edit.rec[byte];
        uint8_t saved[13];
        std::memcpy(saved, g_edit.rec, 13);
        g_edit.rec[byte] = (uint8_t)v[(size_t)cur];
        moa_build_normalise(g_edit.rec);
        text(g, 354.0f, 182.0f, 10.0f, kDim, "WITH THIS PART");
        build_totals(g, 354.0f, 200.0f);
        std::memcpy(g_edit.rec, saved, 13);
        (void)keep;
    }
    footer(g, "A  CHOOSE      B  BACK      L1 / R1  PAGE");
    if (in.back) { g_ai_sub = AI_SUB_BUILD; return; }
    if (in.page_down) cur = std::max(0, cur - visible);
    if (in.page_up) cur = std::min((int)v.size() - 1, cur + visible);
    if (in.confirm) activate = cur;
    if (activate < 0 || activate >= (int)v.size()) return;
    g_edit.rec[byte] = (uint8_t)v[(size_t)activate];
    edit_changed();
    g_ai_sub = AI_SUB_BUILD;
}

/* ---- OPTIONS ---- */

static void draw_ai_opts(const Gfx& g, const Input& in) {
    move_cursor(PAGE_AI_OPTS, MOA_OPTIONS, in, MOA_OPTIONS);
    int& cur = g_state.cursor[PAGE_AI_OPTS];
    struct moa_build_check chk;
    moa_build_check(&g_parts, g_edit.rec, g_edit.plus, &chk);
    const unsigned mask = g_edit.rec[10] | (g_edit.rec[11] << 8);
    const int free_slots = chk.slots_max - chk.slots_used;
    char sub[48];
    std::snprintf(sub, sizeof sub, "%d OF %d SLOTS USED", chk.slots_used, chk.slots_max);
    header(g, "OPTIONAL PARTS", sub);
    panel(g, 24.0f, 62.0f, 380.0f, MOA_OPTIONS * kRowH + 12.0f, "OPTIONS");
    int activate = -1;
    for (int i = 0; i < MOA_OPTIONS; ++i) {
        const bool on = (mask >> i) & 1u;
        const bool fits = on || g_parts.option[i].slots <= free_slots;
        char label[48];
        std::snprintf(label, sizeof label, "%-10s %s", g_parts.option[i].name, g_parts.option[i].what);
        bool hovered = false;
        if (row(g, 25.0f, 74.0f + kRowH * i, 378.0f, i == cur, fits, label, "", true, on, &hovered))
            activate = i;
        if (hovered && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f)) cur = i;
    }
    panel(g, 420.0f, 62.0f, 196.0f, MOA_OPTIONS * kRowH + 12.0f, "WHAT IT DOES");
    text(g, 434.0f, 78.0f, 12.0f, kBright, g_parts.option[cur].name);
    text(g, 434.0f, 96.0f, 10.0f, kDim, g_parts.option[cur].what);
    char b[32];
    std::snprintf(b, sizeof b, "%d SLOT%s", g_parts.option[cur].slots, g_parts.option[cur].slots == 1 ? "" : "S");
    text(g, 434.0f, 112.0f, 10.0f, kHi, b);
    text(g, 434.0f, 134.0f, 10.0f, kText, moa_option_effect(cur), 170.0f);
    footer(g, "A  ON / OFF      B  BACK");
    if (in.back) { g_ai_sub = AI_SUB_BUILD; return; }
    if (in.confirm || in.left || in.right) activate = cur;
    if (activate < 0) return;
    cur = activate;
    const bool on = (mask >> activate) & 1u;
    if (!on && g_parts.option[activate].slots > free_slots) { note("NOT ENOUGH SLOTS ON THIS CORE"); return; }
    const unsigned nm = on ? (mask & ~(1u << activate)) : (mask | (1u << activate));
    g_edit.rec[10] = (uint8_t)(nm & 255);
    g_edit.rec[11] = (uint8_t)(nm >> 8);
    edit_changed();
}

/* ---- PAINT ---- */

static const char* const kSections[4] = { "HEAD", "CORE", "ARMS", "LEGS" };
static const char* const kTones[4] = { "BASE", "OPTIONAL", "DETAIL", "JOINT" };

static bool paint_uniform() {
    for (int i = 12; i < 48; ++i) if (g_edit.paint[i] != g_edit.paint[i % 12]) return false;
    return true;
}

static ImU32 paint_col(int off) {
    const uint8_t* p = g_edit.paint + off;
    return IM_COL32(p[0] * 255 / 64, p[1] * 255 / 64, p[2] * 255 / 64, 255);
}

static void draw_ai_paint(const Gfx& g, const Input& in) {
    enum { P_SAME = 0, P_SECTION, P_C0, P_FROM_EMBLEM = P_C0 + 12, P_DEFAULT, P_ROWS };
    const int visible = 14;
    move_cursor(PAGE_AI_PAINT, P_ROWS, in, visible);
    int& cur = g_state.cursor[PAGE_AI_PAINT];
    const int top = g_state.top[PAGE_AI_PAINT];
    if (!paint_uniform()) g_split_paint = true;
    const bool same = !g_split_paint;
    if (same) g_paint_section = 0;
    header(g, "PAINT", "GAME SLIDER VALUES, 0 TO 64");
    panel(g, 24.0f, 62.0f, 300.0f, visible * kRowH + 12.0f, "PAINT");
    int activate = -1;
    for (int i = top; i < P_ROWS && i < top + visible; ++i) {
        std::string label, value;
        bool enabled = true, toggle = false, on = false;
        if (i == P_SAME) { label = "SAME ON ALL SECTIONS"; toggle = true; on = same; }
        else if (i == P_SECTION) { label = "SECTION"; value = same ? "ALL" : kSections[g_paint_section]; enabled = !same; }
        else if (i < P_FROM_EMBLEM) {
            const int c = i - P_C0;
            char b[32];
            std::snprintf(b, sizeof b, "%s  %c", kTones[c / 3], "RGB"[c % 3]);
            label = b;
            std::snprintf(b, sizeof b, "%d", g_edit.paint[g_paint_section * 12 + c]);
            value = b;
        } else if (i == P_FROM_EMBLEM) label = "PAINT FROM EMBLEM";
        else label = "GAME DEFAULT PAINT";
        bool hovered = false;
        if (row(g, 25.0f, 74.0f + kRowH * (i - top), 298.0f, i == cur, enabled, label.c_str(),
                value.c_str(), toggle, on, &hovered))
            activate = i;
        if (hovered && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f)) cur = i;
        if (i >= P_C0 && i < P_FROM_EMBLEM && (i - P_C0) % 3 == 0) {
            const float y = 74.0f + kRowH * (i - top);
            g.dl->AddRectFilled(at(g, 230.0f, y + 6.0f), at(g, 250.0f, y + 20.0f),
                                paint_col(g_paint_section * 12 + (i - P_C0)));
        }
    }
    panel(g, 340.0f, 62.0f, 276.0f, visible * kRowH + 12.0f, "SCHEME");
    for (int s = 0; s < 4; ++s) {
        const float y = 80.0f + s * 46.0f;
        text(g, 354.0f, y, 10.0f, s == g_paint_section && !same ? kHi : kDim, kSections[s]);
        for (int t = 0; t < 4; ++t) {
            const float x = 354.0f + t * 60.0f;
            g.dl->AddRectFilled(at(g, x, y + 14.0f), at(g, x + 52.0f, y + 34.0f), paint_col(s * 12 + t * 3));
            g.dl->AddRect(at(g, x, y + 14.0f), at(g, x + 52.0f, y + 34.0f), kLine);
        }
    }
    for (int t = 0; t < 4; ++t) text(g, 354.0f + t * 60.0f, 266.0f, 9.0f, kDim, kTones[t]);
    footer(g, "LEFT / RIGHT  1      L1 / R1  8      A  SELECT      B  BACK");

    if (in.back) { g_ai_sub = AI_SUB_BUILD; return; }
    const int step = in.left ? -1 : in.right ? 1 : in.page_down ? -8 : in.page_up ? 8 : 0;
    if (step) {
        if (cur == P_SECTION && !same && (in.left || in.right))
            g_paint_section = (g_paint_section + (in.left ? 3 : 1)) % 4;
        else if (cur >= P_C0 && cur < P_FROM_EMBLEM) {
            const int c = cur - P_C0;
            const int nv = std::clamp((int)g_edit.paint[g_paint_section * 12 + c] + step, 0, 64);
            for (int s = 0; s < 4; ++s)
                if (same || s == g_paint_section) g_edit.paint[s * 12 + c] = (uint8_t)nv;
            g_paint_auto = false;
            g_edit_dirty = true;
        } else if (cur == P_SAME && (in.left || in.right)) activate = P_SAME;
        if (activate < 0) return;
    }
    if (in.confirm) activate = cur;
    if (activate < 0) return;
    cur = activate;
    if (activate == P_SAME) {
        if (same) {                              /* split: sections start equal */
            g_split_paint = true;
            g_paint_section = 0;
        } else {                                 /* every section takes this one's colours */
            for (int i = 0; i < 48; ++i) g_edit.paint[i] = g_edit.paint[g_paint_section * 12 + i % 12];
            g_split_paint = false;
            g_paint_section = 0;
            g_edit_dirty = true;
        }
    } else if (activate == P_FROM_EMBLEM) {
        moa_card_paint_from_emblem(&g_edit);
        g_paint_auto = true;
        g_edit_dirty = true;
    } else if (activate == P_DEFAULT) {
        static const uint8_t paint0[12] = { 0x20, 0x24, 0x1c, 0x10, 0x14, 0x0c,
                                            0x12, 0x18, 0x0c, 0x0c, 0x12, 0x0c };
        for (int i = 0; i < 48; ++i) g_edit.paint[i] = paint0[i % 12];
        g_paint_auto = false;
        g_edit_dirty = true;
    }
}

/* ---- EMBLEM ---- */

static std::string emblem_folder() {
    return g_ai_folder.empty() ? std::string() : (std::filesystem::path(g_ai_folder) / "Emblems").string();
}

static void scan_emblems() {
    g_emblem_files.clear();
    const std::string dir = emblem_folder();
    std::error_code ec;
    if (dir.empty()) return;
    std::filesystem::create_directories(dir, ec);
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        std::string ext = e.path().extension().string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".gif" || ext == ".tga")
            g_emblem_files.push_back(e.path().string());
    }
    std::sort(g_emblem_files.begin(), g_emblem_files.end());
}

static bool load_emblem(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> data;
    if (std::fseek(f, 0, SEEK_END) == 0) {
        const long n = std::ftell(f);
        if (n > 0 && n < 64L * 1024 * 1024 && std::fseek(f, 0, SEEK_SET) == 0) {
            data.resize((size_t)n);
            if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
        }
    }
    std::fclose(f);
    if (data.empty() || !moa_card_emblem_from_image(&g_edit, data.data(), data.size())) return false;
    if (g_paint_auto) moa_card_paint_from_emblem(&g_edit);
    g_edit_dirty = true;
    return true;
}

static void draw_ai_emblem(LauncherModel* m, const Gfx& g, const Input& in) {
    if (!g_emblem_pending.empty()) {
        note(load_emblem(g_emblem_pending) ? "EMBLEM CHANGED" : "THAT FILE COULD NOT BE OPENED AS A PICTURE");
        g_emblem_pending.clear();
    }
    if (g_emblem_files.empty()) scan_emblems();
    const int fixed = 3;                         /* BROWSE, OPEN FOLDER, DEFAULT */
    const int n = fixed + (int)g_emblem_files.size();
    const int visible = 14;
    move_cursor(PAGE_AI_EMBLEM, n, in, visible);
    int& cur = g_state.cursor[PAGE_AI_EMBLEM];
    const int top = g_state.top[PAGE_AI_EMBLEM];
    header(g, "EMBLEM", "64 x 64, 16 COLOURS");
    panel(g, 24.0f, 62.0f, 300.0f, visible * kRowH + 12.0f, "PICTURES");
    int activate = -1;
    for (int i = top; i < n && i < top + visible; ++i) {
        std::string label;
        if (i == 0) label = "BROWSE FOR A PICTURE...";
        else if (i == 1) label = "OPEN THE EMBLEMS FOLDER";
        else if (i == 2) label = "DEFAULT EMBLEM";
        else label = std::filesystem::path(g_emblem_files[(size_t)(i - fixed)]).filename().string();
        bool hovered = false;
        if (row(g, 25.0f, 74.0f + kRowH * (i - top), 298.0f, i == cur, true, label.c_str(), "",
                false, false, &hovered))
            activate = i;
        if (hovered && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f)) cur = i;
    }
    panel(g, 340.0f, 62.0f, 276.0f, visible * kRowH + 12.0f, "EMBLEM");
    draw_emblem(g, 362.0f, 80.0f, 160.0f, g_edit);
    text(g, 354.0f, 256.0f, 10.0f, kText,
         "Put pictures (PNG or JPEG) in the Emblems folder next to your cards, or browse "
         "for one. The centre square is used and cut to the game's 64 x 64 emblem in 16 colours.",
         250.0f);
    footer(g, "A  USE      B  BACK");
    if (in.back) { g_ai_sub = AI_SUB_BUILD; return; }
    if (in.confirm) activate = cur;
    if (activate < 0) return;
    cur = activate;
    if (activate == 0) {
        ui_pick_file(m, "Choose an emblem picture", {"*.png", "*.jpg", "*.jpeg", "*.bmp"},
                     "Picture (.png .jpg .bmp)", [](const char* path) {
                         if (path) g_emblem_pending = path;
                     });
    } else if (activate == 1) {
        const std::string dir = emblem_folder();
        std::error_code ec;
        if (!dir.empty()) std::filesystem::create_directories(dir, ec);
        if (dir.empty() || !moa_menu_open_folder(dir)) note("COULD NOT OPEN THE FOLDER");
        g_emblem_files.clear();
    } else if (activate == 2) {
        moa_card blank;
        moa_card_new(&blank);
        std::memcpy(g_edit.palette, blank.palette, sizeof blank.palette);
        std::memcpy(g_edit.emblem, blank.emblem, sizeof blank.emblem);
        if (g_paint_auto) moa_card_paint_from_emblem(&g_edit);
        g_edit_dirty = true;
    } else {
        note(load_emblem(g_emblem_files[(size_t)(activate - fixed)]) ? "EMBLEM CHANGED"
                                                                     : "THAT FILE COULD NOT BE OPENED AS A PICTURE");
    }
}
