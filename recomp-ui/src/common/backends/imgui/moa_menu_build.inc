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
#include "../../../../../games/master-of-arena/moa_ac_view.h"

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
    { "HEAD", 1, true }, { "CORE", 0, false }, { "ARMS", 2, false }, { "LEGS", 3, false },
    { "BOOSTER", 12, true }, { "FCS", 8, false }, { "GENERATOR", 9, false },
    { "RIGHT ARM", 4, false }, { "LEFT ARM", 5, true }, { "BACK LEFT", 6, true },
    { "BACK RIGHT", 7, true },
};
static const int kSlotCount = (int)(sizeof kSlots / sizeof kSlots[0]);

/* The game's Assembly screen's slots (ASSEMBLY section below). */
static const struct AsmSlot {
    const char* label;
    int byte;
    bool optional;      /* the list starts with No Equipment */
    unsigned wire;      /* MOA_ACV_* kinds drawn as wireframe */
    bool grey;          /* inside the core: grey wireframe */
} kAsm[] = {
    { "HEAD",          1,  true,  1u << MOA_ACV_HEAD,       false },
    { "CORE",          0,  false, 1u << MOA_ACV_CORE,       false },
    { "ARMS",          2,  false, 1u << MOA_ACV_ARMS,       false },
    { "LEGS",          3,  false, 1u << MOA_ACV_LEGS,       false },
    { "ARM WEAPON R",  4,  false, 1u << MOA_ACV_HAND,       false },
    { "ARM WEAPON L",  5,  true,  0,                        false },
    { "BACK WEAPON R", 7,  true,  1u << MOA_ACV_BACK_RIGHT, false },
    { "BACK WEAPON L", 6,  true,  1u << MOA_ACV_BACK_LEFT,  false },
    { "BOOSTERS",      12, true,  1u << MOA_ACV_BOOSTER,    false },
    { "GENERATOR",     9,  false, 1u << MOA_ACV_CORE,       true  },
    { "FCS",           8,  false, 1u << MOA_ACV_CORE,       true  },
};
static const int kAsmCount = (int)(sizeof kAsm / sizeof kAsm[0]);

static int  g_asm_slot = 0;
static bool g_asm_from_build = false;    /* B goes back to the BUILD list */
static bool g_pick_to_asm = false;       /* the slot list was opened from here */

static void open_asm(int slot, bool from_build);

static bool paint_uniform();

#include "moa_menu_asm.inc"

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

/* Two-slot back units (index 52 and up) fill both back slots as one item:
 * the part sits in one back byte, the other stays empty. */
static bool two_slot_back(int id) { return id >= 52 && id != 255; }
static int asm_eff_byte(int byte) {
    if (byte != 6 && byte != 7) return byte;
    const int other = byte == 6 ? 7 : 6;
    return (!two_slot_back(g_edit.rec[byte]) && two_slot_back(g_edit.rec[other])) ? other : byte;
}

static void step_part(int byte, bool optional, int dir) {
    if (slot_locked(byte)) return;
    const std::vector<int> v = slot_choices(byte, optional);
    if (v.empty()) return;
    int at = 0;
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == g_edit.rec[byte]) at = (int)i;
    at = (at + dir + (int)v.size()) % (int)v.size();
    g_edit.rec[byte] = (uint8_t)v[(size_t)at];
    if (byte == 6 || byte == 7) {   /* as the game: a two-slot unit empties the other side */
        const int other = byte == 6 ? 7 : 6;
        if (two_slot_back(g_edit.rec[byte]) || two_slot_back(g_edit.rec[other])) g_edit.rec[other] = 255;
    }
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

/* `ref`: the build as it is, to colour a candidate's totals (green better,
 * red worse); NULL draws them plain. */
static void build_totals(const Gfx& g, float x, float y, const struct moa_build_check* ref = nullptr) {
    struct moa_build_check t;
    moa_build_check(&g_parts, g_edit.rec, g_edit.plus, &t);
    auto vs = [&](int v, int r, bool higher_better) -> ImU32 {
        if (!ref || v == r) return kText;
        return ((v > r) == higher_better) ? kBetter : kWorse;
    };
    char b[48];
    text(g, x, y, 10.0f, kDim, "WEIGHT");
    std::snprintf(b, sizeof b, "%d", t.weight);
    text_right(g, x + 240.0f, y, 10.0f, ref ? vs(t.weight, ref->weight, false) : kText, b);
    text(g, x, y + 14.0f, 10.0f, kDim, "AP");
    std::snprintf(b, sizeof b, "%d", t.ap);
    text_right(g, x + 240.0f, y + 14.0f, 10.0f, ref ? vs(t.ap, ref->ap, true) : kText, b);
    text(g, x, y + 28.0f, 10.0f, kDim, "DEF. SHELL / ENERGY");
    std::snprintf(b, sizeof b, "%d / %d", t.def_shell, t.def_energy);
    text_right(g, x + 240.0f, y + 28.0f, 10.0f,
               ref ? vs(t.def_shell + t.def_energy, ref->def_shell + ref->def_energy, true) : kText, b);
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
       B_SAVE, B_SAVE_NEW, B_MEMCARD, B_ROWS };

/* Opened from the main menu's ASSEMBLY (not the AI Battle card list): back
 * then returns to the main menu. */
static bool g_build_standalone = false;

/* Back to EXTRA GARAGE: the AC goes into the garage, kept for next time. */
static void leave_to_garage() {
    g_garage = g_edit;
    save_garage();
    g_build_standalone = false;
    g_from_garage = false;
    g_ai_sub = AI_SUB_SETUP;
    g_state.ai_page = false;
}

static void leave_build() {
    g_details_open = false;
    if (g_build_standalone) leave_to_garage();
    else g_ai_sub = AI_SUB_CARDS;
}

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
    moa_build_normalise(g_edit.rec);
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
    header(g, g_build_standalone ? "ASSEMBLY" : "AC BUILDER",
           g_edit_path.empty() ? (g_build_standalone ? "NEW AC" : "NEW CARD") : upper(g_edit.ac).c_str());
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
        else if (i == B_SAVE) { label = g_build_standalone ? "KEEP IN GARAGE" : "SAVE CARD"; enabled = !chk.blocked; }
        else if (i == B_SAVE_NEW) { label = g_build_standalone ? "SAVE AS NEW AI CARD" : "SAVE AS NEW CARD"; enabled = !chk.blocked; }
        else { label = "SAVE AS VERSUS UNIT"; value = "VS MODE"; enabled = !chk.blocked && !m->in_session; }
        bool hovered = false;
        if (row(g, 25.0f, 74.0f + kRowH * (i - top), 298.0f, i == cur, enabled, label.c_str(),
                value.c_str(), false, false, &hovered) && !g_name_edit && !g_confirm_discard)
            activate = i;
        if (hovered && !g_name_edit && (ImGui::GetIO().MouseDelta.x != 0.0f ||
                                        ImGui::GetIO().MouseDelta.y != 0.0f))
            cur = i;
    }

    const bool on_slot = cur >= B_SLOT0 && cur < B_OPTS;
    if (g_details_open) {
        draw_details(g, -1);
    } else {
        panel(g, 340.0f, 62.0f, 276.0f, visible * kRowH + 12.0f, "AC");
        draw_acv(g, 341.0f, 75.0f, 274.0f, 168.0f, g_edit);
        draw_emblem(g, 346.0f, 80.0f, 28.0f, g_edit);
        if (on_slot) {
            const int byte = kSlots[cur - B_SLOT0].byte, cat = moa_record_cat(byte);
            const moa_part* p = g_edit.rec[byte] == 255 ? nullptr : moa_parts_find(&g_parts, cat, g_edit.rec[byte]);
            text(g, 354.0f, 250.0f, 12.0f, kBright, p ? p->name : "NONE");
            if (p) text_right(g, 594.0f, 252.0f, 9.0f, kDim, moa_part_group(cat, p->name));
        } else {
            text(g, 354.0f, 250.0f, 12.0f, kBright, upper(g_edit.ac).c_str());
            text_right(g, 594.0f, 252.0f, 9.0f, kHi, ("AI  " + card_ai_label(g_edit)).c_str());
        }
        build_totals(g, 354.0f, 272.0f);
    }
    if (g_name_edit)
        text(g, 354.0f, 400.0f, 10.0f, kHi,
             "UP / DOWN  LETTER   LEFT / RIGHT  MOVE   X  DELETE   A  DONE  (or type)", 250.0f);
    if (g_confirm_discard) {
        panel(g, 360.0f, 330.0f, 236.0f, 64.0f, "UNSAVED CHANGES");
        text(g, 374.0f, 346.0f, 11.0f, kWarn, "LEAVE WITHOUT SAVING?");
        text(g, 374.0f, 370.0f, 10.0f, kText, "A  LEAVE      B  STAY");
    }
    footer(g, on_slot ? "A  ASSEMBLY SCREEN   LEFT / RIGHT  CHANGE   Y / TRIANGLE  DETAILS   B  BACK"
                      : "A  SELECT      LEFT / RIGHT  CHANGE      B  BACK");

    if (g_name_edit) { name_input(in); return; }
    if (g_details_open) {
        if (in.info || in.back || in.confirm) g_details_open = false;
        return;
    }
    if (in.info && on_slot) {
        const int byte = kSlots[cur - B_SLOT0].byte;
        open_details(moa_record_cat(byte), g_edit.rec[byte]);
        return;
    }
    if (g_confirm_discard) {
        if (in.back) { g_confirm_discard = false; return; }
        if (in.confirm) { g_confirm_discard = false; g_edit_dirty = false; leave_build(); }
        return;
    }
    if (in.back && !in.info) {
        if (g_build_standalone) { open_asm(g_asm_slot, false); return; }   /* back to the Assembly screen */
        if (g_edit_dirty) g_confirm_discard = true; else leave_build();
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
        /* the game's Assembly screen, on this slot */
        const int byte = kSlots[activate - B_SLOT0].byte;
        int slot = 0;
        for (int i = 0; i < kAsmCount; ++i) if (kAsm[i].byte == byte) slot = i;
        open_asm(slot, true);
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
    } else if (activate == B_SAVE && g_build_standalone) {
        g_garage = g_edit;
        note(save_garage() ? "KEPT IN THE GARAGE" : "THE GARAGE AC COULD NOT BE WRITTEN");
        g_edit_dirty = false;
    } else if (activate == B_SAVE) save_build(m, false);
    else if (activate == B_SAVE_NEW) {
        save_build(m, true);
        if (g_build_standalone) g_edit_path = garage_path();   /* still the garage AC */
    }
    else if (activate == B_MEMCARD) {
        if (build_blocked()) note("THE GAME WILL NOT FIELD THIS AC: FIX THE BUILD FIRST");
        else save_to_memcard(m);
    }
}

/* ---- ASSEMBLY: the game's own screen ----
 *
 * As the garage shows it: up / down picks the slot, left / right changes
 * the part in it straight away. Left window: the slot name in the orange
 * bar, that part on its own turning, its EP / WP and NAME. Right window:
 * the whole AC turning, the slot being changed as a wireframe (red; grey for
 * the generator and FCS, which sit inside the core), and the totals the
 * garage keeps live: EP, LEGS WP, CORE WP, AP, WEIGHT. TRIANGLE lays the
 * part's details page over both windows; it follows the part while you
 * scroll. A opens the slot's full list, zoom is the right stick.
 */

static const ImU32 kGameBlue = IM_COL32(72, 80, 176, 255);
static const ImU32 kWireRed  = IM_COL32(200, 36, 36, 255);
static const ImU32 kWireGrey = IM_COL32(176, 176, 176, 255);

static void open_asm(int slot, bool from_build) {
    g_asm_slot = std::clamp(slot, 0, kAsmCount - 1);
    g_asm_from_build = from_build;
    g_details_open = false;
    g_ai_sub = AI_SUB_ASM;
}

/* The slot's part: none for 255, and for the hand slot its "NO WEAPON". */
static bool asm_empty(int byte, int id) { return id == 255 || (byte == 4 && id == 0); }

static void game_box(const Gfx& g, float x, float y, float w, float h) {
    g.dl->AddRectFilled(at(g, x, y), at(g, x + w, y + h), IM_COL32(0, 0, 0, 255));
    g.dl->AddRect(at(g, x, y), at(g, x + w, y + h), kGameBlue, 0.0f, 0, 2.0f * g.s);
}

/* The garage's orange title bar. */
static void game_bar(const Gfx& g, float x, float y, float w, float h, const char* label) {
    const ImU32 dark = IM_COL32(96, 48, 12, 255), mid = IM_COL32(222, 140, 56, 255);
    g.dl->AddRectFilledMultiColor(at(g, x, y), at(g, x + w, y + h * 0.5f), dark, dark, mid, mid);
    g.dl->AddRectFilledMultiColor(at(g, x, y + h * 0.5f), at(g, x + w, y + h), mid, mid, dark, dark);
    const float tw = text_width(g, 14.0f, label);
    text(g, x + (w - tw) * 0.5f, y + (h - 14.0f) * 0.5f - 1.0f, 14.0f, kBright, label);
}

static void game_arrow(const Gfx& g, float cx, float cy, int dir, bool lit) {
    const ImU32 c = lit ? IM_COL32(232, 150, 48, 255) : IM_COL32(110, 70, 30, 255);
    const float r = 9.0f;
    switch (dir) {
    case 0: g.dl->AddTriangleFilled(at(g, cx, cy - r * 0.6f), at(g, cx + r, cy + r * 0.6f), at(g, cx - r, cy + r * 0.6f), c); break;
    case 1: g.dl->AddTriangleFilled(at(g, cx - r, cy - r * 0.6f), at(g, cx + r, cy - r * 0.6f), at(g, cx, cy + r * 0.6f), c); break;
    case 2: g.dl->AddTriangleFilled(at(g, cx + r * 0.6f, cy - r), at(g, cx + r * 0.6f, cy + r), at(g, cx - r * 0.6f, cy), c); break;
    default: g.dl->AddTriangleFilled(at(g, cx - r * 0.6f, cy - r), at(g, cx + r * 0.6f, cy), at(g, cx - r * 0.6f, cy + r), c); break;
    }
}

static void stat_pair(const Gfx& g, float x, float right, float y, const char* label, int a, int b, bool over) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%d / %d", a, b);
    text(g, x, y, 11.0f, kText, label);
    text_right(g, right, y, 11.0f, over ? kWarn : kText, buf);
}

/* The details page laid over both windows (TRIANGLE). */
static void draw_asm_details(const Gfx& g, const char* slot_label, int cat, int id) {
    const float x = 92.0f, y = 74.0f, w = 456.0f;
    moa_acv_details d;
    const bool have = !asm_empty(kAsm[g_asm_slot].byte, id) && part_details(cat, id, &d);
    const int rows = have ? d.count + 2 : 1;
    const float h = 30.0f + 14.0f * (float)rows;
    g.dl->AddRectFilled(at(g, x, y), at(g, x + w, y + h), IM_COL32(14, 22, 92, 205));
    g.dl->AddRect(at(g, x, y), at(g, x + w, y + h), IM_COL32(96, 104, 210, 255), 0.0f, 0, 1.5f * g.s);
    const float tw = text_width(g, 14.0f, slot_label);
    text(g, x + (w - tw) * 0.5f, y + 4.0f, 14.0f, IM_COL32(150, 150, 220, 150), slot_label);
    const float lx = x + 18.0f, vx = x + 232.0f;
    float yy = y + 24.0f;
    if (!have) {
        text(g, lx, yy, 11.0f, kText, asm_empty(kAsm[g_asm_slot].byte, id) ? "NO EQUIPMENT" : "NO DETAILS FOR THIS PART");
        return;
    }
    auto line = [&](const char* label, const char* value) {
        text(g, lx, yy, 11.0f, kBright, label);
        text(g, vx, yy, 11.0f, kBright, value);
        yy += 14.0f;
    };
    line("NAME", d.name);
    line("TYPE", d.type);
    char b[32];
    for (int i = 0; i < d.count; ++i) {
        if (d.f[i].is_text) line(d.f[i].label, d.f[i].text);
        else { std::snprintf(b, sizeof b, "%ld", d.f[i].value); line(d.f[i].label, b); }
    }
}

static void draw_ai_asm(LauncherModel* m, const Gfx& g, const Input& in) {
    header(g, "ASSEMBLY", upper(g_edit.ac[0] ? g_edit.ac : "AC").c_str());
    if (!parts_ready(m)) {
        panel(g, 24.0f, 76.0f, 592.0f, 120.0f, "PARTS");
        text(g, 38.0f, 96.0f, 12.0f, kWarn, "THE PART LIST COULD NOT BE READ FROM THE GAME DISC.");
        footer(g, "B  BACK");
        if (in.back) { g_parts_state = 0; leave_build(); }
        return;
    }
    const AsmSlot& sl = kAsm[g_asm_slot];
    const int byte = asm_eff_byte(sl.byte), cat = moa_record_cat(byte), id = g_edit.rec[byte];
    unsigned wire = sl.wire;
    if (two_slot_back(id)) wire = (1u << MOA_ACV_BACK_LEFT) | (1u << MOA_ACV_BACK_RIGHT);
    const bool locked = slot_locked(byte);
    const bool empty = asm_empty(byte, id);
    const moa_part* p = empty ? nullptr : moa_parts_find(&g_parts, cat, (uint8_t)id);
    struct moa_build_check chk;
    moa_build_check(&g_parts, g_edit.rec, g_edit.plus, &chk);

    /* left window: the slot and its part */
    const float lx = 24.0f, rx = 326.0f, top = 60.0f, w = 290.0f, h = 376.0f;
    game_box(g, lx, top, w, h);
    game_bar(g, lx + 4.0f, top + 4.0f, w - 8.0f, 24.0f, sl.label);
    game_arrow(g, lx + w * 0.5f, top + 42.0f, 0, true);
    game_arrow(g, lx + w * 0.5f, top + 236.0f, 1, true);
    game_arrow(g, lx + 16.0f, top + 140.0f, 2, !locked);
    game_arrow(g, lx + w - 16.0f, top + 140.0f, 3, !locked);
    const float vx = lx + 32.0f, vy = top + 52.0f, vw = w - 64.0f, vh = 176.0f;
    if (locked) {
        const char* t = "NOT WITH WEAPON ARMS";
        if (byte == 12) t = "NOT WITH CATERPILLAR LEGS";
        text(g, lx + (w - text_width(g, 11.0f, t)) * 0.5f, vy + vh * 0.5f - 6.0f, 11.0f, kDim, t);
    } else if (empty) {
        const char* t = "No Equipment";
        const float tw = text_width(g, 20.0f, t);
        text(g, lx + (w - tw) * 0.5f + 1.5f, vy + vh * 0.5f - 9.0f, 20.0f, IM_COL32(200, 60, 0, 160), t);
        text(g, lx + (w - tw) * 0.5f, vy + vh * 0.5f - 10.0f, 20.0f, IM_COL32(255, 196, 64, 255), t);
    } else if (!draw_part_view(g, vx, vy, vw, vh, cat, id, g_edit.paint)) {
        const char* t = !models_installed() ? "EXTRAS > INSTALL 3D AC MODELS"
                                            : "INSTALL THE 3D MODELS AGAIN (EXTRAS) FOR PART PICTURES";
        text(g, vx + 4.0f, vy + vh * 0.5f - 6.0f, 10.0f, kDim, t, vw - 8.0f);
    }
    g.dl->AddLine(at(g, lx, top + 252.0f), at(g, lx + w, top + 252.0f), kGameBlue, 1.5f * g.s);
    char b[48];
    text(g, lx + 14.0f, top + 262.0f, 11.0f, kText, "EP");
    text(g, lx + 14.0f, top + 276.0f, 11.0f, kText, "WP");
    if (p) {
        std::snprintf(b, sizeof b, "%d", cat == MOA_CAT_GEN ? 0 : p->drain);
        text_right(g, lx + w - 70.0f, top + 262.0f, 11.0f, kText, b);
        std::snprintf(b, sizeof b, "%d", p->weight);
        text_right(g, lx + w - 70.0f, top + 276.0f, 11.0f, kText, b);
    } else {
        text_right(g, lx + w - 70.0f, top + 262.0f, 11.0f, kText, "------");
        text_right(g, lx + w - 70.0f, top + 276.0f, 11.0f, kText, "------");
    }
    g.dl->AddLine(at(g, lx, top + 336.0f), at(g, lx + w, top + 336.0f), kGameBlue, 1.5f * g.s);
    text(g, lx + 14.0f, top + 350.0f, 11.0f, kText, "NAME");
    text(g, lx + 70.0f, top + 350.0f, 11.0f, kText, locked ? "-" : p ? p->name : "NO EQUIPMENT");

    /* right window: the AC, the slot as a wireframe, the totals */
    game_box(g, rx, top, w, h);
    draw_acv(g, rx + 2.0f, top + 2.0f, w - 4.0f, 248.0f, g_edit, wire, sl.grey ? kWireGrey : kWireRed, true);
    g.dl->AddLine(at(g, rx, top + 252.0f), at(g, rx + w, top + 252.0f), kGameBlue, 1.5f * g.s);
    const float sx = rx + 14.0f, sr = rx + w - 14.0f;
    stat_pair(g, sx, sr, top + 262.0f, "EP", chk.drain, chk.output, chk.energy_over);
    stat_pair(g, sx, sr, top + 276.0f, "LEGS WP", chk.leg_load, chk.leg_max, chk.leg_over);
    stat_pair(g, sx, sr, top + 290.0f, "CORE WP", chk.arm_load, chk.arm_max, chk.arm_over);
    text(g, sx, top + 308.0f, 11.0f, kText, "AP");
    std::snprintf(b, sizeof b, "%d", chk.ap);
    text_right(g, sr, top + 308.0f, 11.0f, kText, b);
    text(g, sx, top + 322.0f, 11.0f, kText, "WEIGHT");
    std::snprintf(b, sizeof b, "%d", chk.weight);
    text_right(g, sr, top + 322.0f, 11.0f, kText, b);
    g.dl->AddLine(at(g, rx, top + 336.0f), at(g, rx + w, top + 336.0f), kGameBlue, 1.5f * g.s);
    const char* verdict = nullptr;
    ImU32 vcol = kWarn;
    if (chk.missing_booster) verdict = "THESE LEGS NEED A BOOSTER.";
    else if (chk.slots_over) verdict = "THE OPTIONAL PARTS NEED MORE SLOTS THAN THIS CORE HAS.";
    else if (chk.blocked) verdict = "OVER THE LIMITS: THE GAME WILL NOT FIELD THIS AC WITHOUT HUMAN PLUS.";
    else if (chk.leg_over || chk.arm_over || chk.energy_over) { verdict = "OVER THE LIMITS, ALLOWED WITH HUMAN PLUS."; vcol = kHi; }
    if (verdict) text(g, sx, top + 344.0f, 10.0f, vcol, verdict, w - 28.0f);

    if (g_details_open) draw_asm_details(g, sl.label, cat, id);
    footer(g, "UP/DOWN  SLOT   LEFT/RIGHT  PART   Y/TRIANGLE  DETAILS   R-STICK  ZOOM   B  BACK");

    if (in.info) { g_details_open = !g_details_open; return; }
    if (in.back) {
        if (g_details_open) { g_details_open = false; return; }
        if (g_asm_from_build) { g_ai_sub = AI_SUB_BUILD; return; }
        leave_build();
        return;
    }
    if (in.up) g_asm_slot = (g_asm_slot + kAsmCount - 1) % kAsmCount;
    if (in.down) g_asm_slot = (g_asm_slot + 1) % kAsmCount;
    if (in.left || in.right) {
        if (byte != sl.byte) { g_edit.rec[sl.byte] = g_edit.rec[byte]; g_edit.rec[byte] = 255; }
        step_part(sl.byte, sl.optional, in.left ? -1 : 1);
    }
}

/* ---- PICK ---- */

/* The game's own type text for a part (SMALL MISSILE, CHAIN GUN, RADAR...),
 * from the install's part files; the builder's grouping without them. */
static std::string part_type_text(int cat, int id) {
    static std::map<int, std::string> cache;
    static int cache_state = 0;
    if (cache_state != g_models_state) { cache.clear(); cache_state = g_models_state; }
    const int key = cat * 256 + id;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::string t;
    moa_acv_details d;
    if (models_installed() && moa_acv_part_details(models_dir().c_str(), cat, id, &d)) t = d.type;
    if (t.empty()) {
        const moa_part* p = moa_parts_find(&g_parts, cat, (uint8_t)id);
        if (p) t = moa_part_group(cat, p->name);
    }
    cache[key] = t;
    return t;
}


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
        if (marked(byte, v[(size_t)i])) label = "> " + label;
        bool hovered = false;
        const std::string grp = p ? part_type_text(cat, v[(size_t)i]) : std::string();
        if (row(g, 25.0f, 74.0f + kRowH * (i - top), 298.0f, i == cur, true, label.c_str(),
                grp.c_str(), false, false, &hovered))
            activate = i;
        if (hovered && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f)) cur = i;
    }
    const int at_id = (cur >= 0 && cur < (int)v.size()) ? v[(size_t)cur] : 255;
    if (g_details_open) {
        draw_details(g, g_edit.rec[byte]);
    } else if (!g_marks[byte].empty()) {
        draw_compare(g, byte);
    } else if (cur >= 0 && cur < (int)v.size()) {
        panel(g, 340.0f, 62.0f, 276.0f, visible * kRowH + 12.0f, "PART");
        const moa_part* p = at_id == 255 ? nullptr : moa_parts_find(&g_parts, cat, (uint8_t)at_id);
        struct moa_build_check before;
        moa_build_check(&g_parts, g_edit.rec, g_edit.plus, &before);
        /* the build with this part in: the AC and its totals */
        moa_card preview = g_edit;
        preview.rec[byte] = (uint8_t)at_id;
        moa_build_normalise(preview.rec);
        draw_acv(g, 341.0f, 75.0f, 274.0f, 168.0f, preview);
        text(g, 354.0f, 250.0f, 12.0f, kBright, p ? p->name : "NONE");
        if (p) text_right(g, 594.0f, 252.0f, 9.0f, kDim, moa_part_group(cat, p->name));
        uint8_t saved[13];
        std::memcpy(saved, g_edit.rec, 13);
        std::memcpy(g_edit.rec, preview.rec, 13);
        text(g, 354.0f, 268.0f, 9.0f, kDim, "WITH THIS PART");
        build_totals(g, 354.0f, 282.0f, &before);
        std::memcpy(g_edit.rec, saved, 13);
    }
    footer(g, "A  CHOOSE   Y / TRIANGLE  DETAILS   X / SQUARE  MARK TO COMPARE   L1 / R1  PAGE   B  BACK");
    if (g_details_open) {
        if (in.info || in.back || in.confirm) g_details_open = false;
        return;
    }
    if (in.info) { open_details(cat, at_id); return; }   /* TRIANGLE is not BACK here */
    if (in.alt) { toggle_mark(byte, at_id); return; }
    if (in.back) { g_ai_sub = g_pick_to_asm ? AI_SUB_ASM : AI_SUB_BUILD; g_pick_to_asm = false; return; }
    if (in.page_down) cur = std::max(0, cur - visible);
    if (in.page_up) cur = std::min((int)v.size() - 1, cur + visible);
    if (in.confirm) activate = cur;
    if (activate < 0 || activate >= (int)v.size()) return;
    g_edit.rec[byte] = (uint8_t)v[(size_t)activate];
    edit_changed();
    g_ai_sub = g_pick_to_asm ? AI_SUB_ASM : AI_SUB_BUILD;
    g_pick_to_asm = false;
}

/* ---- OPTIONAL PARTS: the game's screen ----
 *
 * As the garage shows it: the optional parts down the left, the highlighted
 * one in the orange bar, a lit box beside the ones fitted. Right: that part
 * alone, turning (models/menu/option/NN), its type, the slots it needs and
 * the slots the core has free; the description line runs along the bottom.
 * HUMAN PLUS sits under the list as OFF / FULL.
 */

static void draw_ai_opts(const Gfx& g, const Input& in) {
    const int rows = MOA_OPTIONS + 1;              /* + HUMAN PLUS */
    const int plus_row = MOA_OPTIONS;
    int& cur = g_state.cursor[PAGE_AI_OPTS];
    if (in.up) cur = (cur + rows - 1) % rows;
    if (in.down) cur = (cur + 1) % rows;
    if (cur < 0 || cur >= rows) cur = 0;
    struct moa_build_check chk;
    moa_build_check(&g_parts, g_edit.rec, g_edit.plus, &chk);
    const unsigned mask = g_edit.rec[10] | (g_edit.rec[11] << 8);
    const int free_slots = chk.slots_max - chk.slots_used;
    header(g, "OPTIONAL PARTS", upper(g_edit.ac[0] ? g_edit.ac : "AC").c_str());

    /* the list */
    const float lx = 24.0f, top = 60.0f, lw = 290.0f, rh = 24.0f;
    game_box(g, lx, top, lw, rh * rows + 12.0f);
    int activate = -1;
    for (int i = 0; i < rows; ++i) {
        const float y = top + 6.0f + rh * i;
        const bool plus = i == plus_row;
        const bool on = plus ? g_edit.plus != 0 : ((mask >> i) & 1u) != 0;
        const bool fits = plus || on || g_parts.option[i].slots <= free_slots;
        /* the fitted box */
        g.dl->AddRect(at(g, lx + 8.0f, y + 3.0f), at(g, lx + 30.0f, y + rh - 3.0f), kGameBlue, 0.0f, 0, 1.5f * g.s);
        if (on) g.dl->AddRectFilled(at(g, lx + 12.0f, y + 7.0f), at(g, lx + 26.0f, y + rh - 7.0f),
                                    IM_COL32(232, 150, 48, 255));
        const char* name = plus ? "HUMAN PLUS" : g_parts.option[i].name;
        if (i == cur) game_bar(g, lx + 36.0f, y + 1.0f, lw - 44.0f, rh - 2.0f, name);
        else text(g, lx + 44.0f, y + 5.0f, 12.0f, fits ? kText : kDim, name);
        if (plus) text_right(g, lx + lw - 12.0f, y + 6.0f, 10.0f, i == cur ? kBright : kHi, plus_text(g_edit.plus).c_str());
        const ImVec2 r0 = at(g, lx, y), r1 = at(g, lx + lw, y + rh);
        if (ImGui::IsMouseHoveringRect(r0, r1)) {
            if (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f) cur = i;
            if (ImGui::IsMouseClicked(0)) activate = i;
        }
    }

    /* the part alone */
    const float rx = 326.0f, rw = 290.0f;
    game_box(g, rx, top, rw, 200.0f);
    char b[48];
    if (cur == plus_row) {
        const char* t = "HUMAN PLUS";
        text(g, rx + (rw - text_width(g, 20.0f, t)) * 0.5f, top + 86.0f, 20.0f, kHi, t);
    } else if (!draw_kind_view(g, rx + 4.0f, top + 4.0f, rw - 8.0f, 192.0f, "option", cur, g_edit.paint)) {
        const char* t = !models_installed() ? "EXTRAS > INSTALL 3D AC MODELS"
                                            : "INSTALL THE 3D MODELS AGAIN (EXTRAS) FOR PART PICTURES";
        text(g, rx + 14.0f, top + 92.0f, 10.0f, kDim, t, rw - 28.0f);
    }
    game_box(g, rx, top + 206.0f, rw, 62.0f);
    if (cur == plus_row) {
        text(g, rx + 14.0f, top + 214.0f, 11.0f, kText, "PILOT OPERATION");
        text(g, rx + 14.0f, top + 230.0f, 11.0f, kText, ("NOW  " + plus_text(g_edit.plus)).c_str());
        text(g, rx + 14.0f, top + 246.0f, 11.0f, kText, "NO SLOTS USED");
    } else {
        text(g, rx + 14.0f, top + 214.0f, 11.0f, kText, upper(g_parts.option[cur].what).c_str());
        std::snprintf(b, sizeof b, "%d SLOT%s  REQUIRED", g_parts.option[cur].slots, g_parts.option[cur].slots == 1 ? "" : "S");
        text(g, rx + 14.0f, top + 230.0f, 11.0f, kText, b);
        std::snprintf(b, sizeof b, "%d SLOT%s  AVAILABLE", free_slots, free_slots == 1 ? "" : "S");
        text(g, rx + 14.0f, top + 246.0f, 11.0f, free_slots < 0 ? kWarn : kText, b);
    }
    /* the description line */
    game_box(g, lx, top + rh * rows + 20.0f, 592.0f, 26.0f);
    text(g, lx + 10.0f, top + rh * rows + 26.0f, 11.0f, kText,
         cur == plus_row ? "Lets the AC go over the load and energy limits. OFF or FULL."
                         : moa_option_effect(cur), 572.0f);

    footer(g, "UP/DOWN  PART      A / LEFT / RIGHT  FIT / REMOVE      B  BACK");
    if (in.back) {
        if (g_from_garage) leave_to_garage(); else g_ai_sub = AI_SUB_BUILD;
        return;
    }
    if (in.confirm || in.left || in.right) activate = cur;
    if (activate < 0) return;
    cur = activate;
    if (activate == plus_row) {
        g_edit.plus = g_edit.plus ? 0 : 9;      /* full = 9 */
        g_edit_dirty = true;
        return;
    }
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

    if (in.back) { if (g_from_garage) leave_to_garage(); else g_ai_sub = AI_SUB_BUILD; return; }
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
    if (in.back) { if (g_from_garage) leave_to_garage(); else g_ai_sub = AI_SUB_BUILD; return; }
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
