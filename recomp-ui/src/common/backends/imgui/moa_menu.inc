/* Master of Arena system menu: a controller-first front end for the launcher.
 *
 * Included by launcher_imgui.cpp when RECOMP_LAUNCHER_MOA_MENU is set. It owns
 * the launcher's dashboard, settings, mods and netplay views (the network
 * pages are in moa_menu_net.inc) and draws them as one list at a time: D-pad
 * to move, one button to confirm or toggle, one to go back. Everything else
 * (the controller page, online sign-in, every modal) is the stock launcher,
 * reached from here and returned from with its own Back.
 *
 * It draws straight onto the launcher window's draw list on a 640x480 design
 * grid scaled to the window, and reads input through ImGui's key state, so it
 * needs nothing from the platform layer. All game state goes through the
 * launcher model's own functions and the mod provider; this file keeps only
 * cursor positions.
 *
 * Expected from the including file: LauncherModel, LauncherTheme, asset(),
 * ui_pick_file(), request_mod_commit_launch(), mod_note_error(),
 * moa_menu_open_folder(), and what moa_menu_net.inc lists.
 */

#include "../../../../../games/master-of-arena/moa_card.h"
#include "../../../../../games/master-of-arena/moa_phantasma.h"
#include "../../../../../games/master-of-arena/moa_update.h"

namespace moa_menu {

enum Page {
    PAGE_MAIN = 0, PAGE_SETTINGS, PAGE_MODS,
    PAGE_NET_MODE, PAGE_NET_ROOMS, PAGE_LOBBY, PAGE_AI, PAGE_EXTRAS, PAGE_UPDATES,
    PAGE_AI_CARDS, PAGE_AI_EDIT, PAGE_AI_ADV, PAGE_AI_BUILD, PAGE_AI_PICK, PAGE_AI_OPTS,
    PAGE_AI_PAINT, PAGE_AI_EMBLEM, PAGE__COUNT
};

struct State {
    int    cursor[PAGE__COUNT] = {0};
    int    top[PAGE__COUNT] = {0};
    bool   classic_settings = false;   /* the stock page was asked for */
    bool   classic_mods = false;
    bool   classic_net = false;
    bool   net_ready = false;          /* network settings loaded this run */
    double signin_since = 0.0;         /* waiting on a saved online sign-in */
    double next_list = 0.0;            /* next room list request */
    bool   direct = false;             /* the join-by-address editor is up */
    int    direct_ip[4] = {192, 168, 1, 1};
    int    direct_port = 7777;
    int    direct_digit = 9;           /* which of its 17 digits is chosen */
    bool   recent_loaded = false;
    std::vector<std::string> recent;   /* addresses joined before, newest first */
    int    last_view = -1;
    double next_scan = 0.0;
    std::vector<std::string> new_packages;   /* files waiting in mods/new */
    bool   ai_page = false;            /* the AI Battle page is up */
    bool   extras_page = false;        /* the Extras page is up */
    bool   updates_page = false;       /* the Updates page is up */
    char   note[200] = {0};
    double note_until = 0.0;
};
static State g_state;

/* Design grid. */
static const float kW = 640.0f, kH = 480.0f;
static const float kRowH = 26.0f;

static const ImU32 kBg      = IM_COL32(6, 9, 13, 255);
static const ImU32 kGrid    = IM_COL32(26, 43, 51, 110);
static const ImU32 kPanel   = IM_COL32(11, 18, 24, 225);
static const ImU32 kLine    = IM_COL32(47, 74, 85, 255);
static const ImU32 kText    = IM_COL32(185, 212, 207, 255);
static const ImU32 kBright  = IM_COL32(255, 244, 220, 255);
static const ImU32 kDim     = IM_COL32(95, 127, 128, 255);
static const ImU32 kHi      = IM_COL32(240, 160, 32, 255);
static const ImU32 kOn      = IM_COL32(95, 224, 138, 255);
static const ImU32 kOff     = IM_COL32(61, 84, 88, 255);
static const ImU32 kWarn    = IM_COL32(226, 91, 58, 255);
static const ImU32 kOnText  = IM_COL32(6, 17, 10, 255);

struct Gfx {
    ImDrawList* dl;
    ImFont*     font;
    float       s;      /* window pixels per design unit */
    ImVec2      o;      /* window position of design (0,0) */
};

static ImVec2 at(const Gfx& g, float x, float y) {
    return ImVec2(g.o.x + x * g.s, g.o.y + y * g.s);
}

static float text_width(const Gfx& g, float size, const char* s) {
    return g.font->CalcTextSizeA(size * g.s, FLT_MAX, 0.0f, s).x / g.s;
}

static void text(const Gfx& g, float x, float y, float size, ImU32 color,
                 const char* s, float wrap = 0.0f) {
    g.dl->AddText(g.font, size * g.s, at(g, x, y), color, s, nullptr,
                  wrap > 0.0f ? wrap * g.s : 0.0f);
}

static void text_right(const Gfx& g, float right, float y, float size,
                       ImU32 color, const char* s) {
    text(g, right - text_width(g, size, s), y, size, color, s);
}

static std::string upper(const char* s) {
    std::string out(s ? s : "");
    for (char& c : out) c = (char)std::toupper((unsigned char)c);
    return out;
}

static bool same_word(const char* a, const char* b) {
    if (!a || !b) return false;
    for (; *a && *b; ++a, ++b)
        if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b))
            return false;
    return *a == *b;
}

static void panel(const Gfx& g, float x, float y, float w, float h,
                  const char* caption) {
    g.dl->AddRectFilled(at(g, x, y), at(g, x + w, y + h), kPanel);
    g.dl->AddRect(at(g, x, y), at(g, x + w, y + h), kLine);
    if (caption && caption[0]) {
        const float cw = text_width(g, 10.0f, caption);
        g.dl->AddRectFilled(at(g, x + 10.0f, y - 6.0f),
                            at(g, x + 22.0f + cw, y + 6.0f), kBg);
        text(g, x + 16.0f, y - 6.0f, 10.0f, kDim, caption);
    }
}

static void header(const Gfx& g, const char* title, const char* sub) {
    text(g, 24.0f, 22.0f, 19.0f, IM_COL32(223, 238, 234, 255), title);
    text_right(g, kW - 24.0f, 32.0f, 10.0f, kDim, sub);
    g.dl->AddLine(at(g, 24.0f, 52.0f), at(g, kW - 24.0f, 52.0f), kLine);
}

static void footer(const Gfx& g, const char* hints) {
    g.dl->AddLine(at(g, 24.0f, kH - 36.0f), at(g, kW - 24.0f, kH - 36.0f), kLine);
    text(g, 24.0f, kH - 28.0f, 10.0f, kDim, hints);
    if (g_state.note[0] && ImGui::GetTime() < g_state.note_until)
        text_right(g, kW - 24.0f, kH - 28.0f, 10.0f, kHi, g_state.note);
}

static void note(const char* s) {
    std::snprintf(g_state.note, sizeof g_state.note, "%s", s ? s : "");
    g_state.note_until = ImGui::GetTime() + 4.0;
}

static void lamp(const Gfx& g, float right, float y, bool on, const char* label) {
    const float w = 38.0f, h = 16.0f;
    const ImVec2 a = at(g, right - w, y), b = at(g, right, y + h);
    if (on) g.dl->AddRectFilled(a, b, kOn);
    g.dl->AddRect(a, b, on ? kOn : kOff);
    const float tw = text_width(g, 11.0f, label);
    text(g, right - w + (w - tw) * 0.5f, y + 2.0f, 11.0f, on ? kOnText : kOff, label);
}

/* One selectable row; returns true when it was clicked with the mouse. */
static bool row(const Gfx& g, float x, float y, float w, bool selected,
                bool enabled, const char* label, const char* value,
                bool is_toggle, bool toggle_on, bool* hovered_out) {
    const ImVec2 a = at(g, x, y), b = at(g, x + w, y + kRowH);
    const bool hovered = ImGui::IsMouseHoveringRect(a, b, false);
    if (hovered_out) *hovered_out = hovered;
    if (selected) {
        g.dl->AddRectFilledMultiColor(a, b, IM_COL32(240, 160, 32, 70),
                                      IM_COL32(240, 160, 32, 0),
                                      IM_COL32(240, 160, 32, 0),
                                      IM_COL32(240, 160, 32, 70));
        g.dl->AddTriangleFilled(at(g, x + 12.0f, y + 8.0f),
                                at(g, x + 21.0f, y + 13.0f),
                                at(g, x + 12.0f, y + 18.0f), kHi);
    }
    const ImU32 color = !enabled ? kOff : selected ? kBright : kText;
    text(g, x + 30.0f, y + 6.0f, 13.0f, color, label);
    if (is_toggle) {
        lamp(g, x + w - 14.0f, y + 5.0f, toggle_on && enabled,
             !enabled ? "N/A" : toggle_on ? "ON" : "OFF");
    } else if (value && value[0]) {
        text_right(g, x + w - 14.0f, y + 6.0f, 13.0f, enabled ? kHi : kOff, value);
    }
    return hovered && enabled && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

/* ---- input ---------------------------------------------------------------- */

struct Input {
    bool up, down, left, right, confirm, back, alt, start, page_up, page_down;
};

static bool key(ImGuiKey k, bool repeat) { return ImGui::IsKeyPressed(k, repeat); }

static Input read_input(const LauncherModel* m) {
    Input in = {};
    /* A modal owns the screen, a text field owns the keys, and a rebind
     * capture owns the pad. */
    if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                    ImGuiPopupFlags_AnyPopupLevel) ||
        ImGui::GetIO().WantTextInput || m->capturing || m->hk_capturing ||
        m->camera_capturing || m->setup_wizard_open)
        return in;
    in.up    = key(ImGuiKey_UpArrow, true) || key(ImGuiKey_GamepadDpadUp, true) ||
               key(ImGuiKey_GamepadLStickUp, true);
    in.down  = key(ImGuiKey_DownArrow, true) || key(ImGuiKey_GamepadDpadDown, true) ||
               key(ImGuiKey_GamepadLStickDown, true);
    in.left  = key(ImGuiKey_LeftArrow, true) || key(ImGuiKey_GamepadDpadLeft, true) ||
               key(ImGuiKey_GamepadLStickLeft, true);
    in.right = key(ImGuiKey_RightArrow, true) || key(ImGuiKey_GamepadDpadRight, true) ||
               key(ImGuiKey_GamepadLStickRight, true);
    in.confirm = key(ImGuiKey_Enter, false) || key(ImGuiKey_KeypadEnter, false) ||
                 key(ImGuiKey_Space, false) || key(ImGuiKey_GamepadFaceDown, false);
    in.back  = key(ImGuiKey_Escape, false) || key(ImGuiKey_Backspace, false) ||
               key(ImGuiKey_GamepadFaceRight, false) ||
               key(ImGuiKey_GamepadFaceUp, false);
    in.alt   = key(ImGuiKey_Delete, false) || key(ImGuiKey_GamepadFaceLeft, false);
    in.start = key(ImGuiKey_GamepadStart, false);
    in.page_up   = key(ImGuiKey_PageUp, true) || key(ImGuiKey_GamepadR1, true);
    in.page_down = key(ImGuiKey_PageDown, true) || key(ImGuiKey_GamepadL1, true);
    return in;
}

static void move_cursor(Page page, int count, const Input& in, int visible) {
    int& c = g_state.cursor[page];
    int& t = g_state.top[page];
    if (count <= 0) { c = t = 0; return; }
    if (in.up)   c = (c + count - 1) % count;
    if (in.down) c = (c + 1) % count;
    if (c < 0) c = 0;
    if (c >= count) c = count - 1;
    if (c < t) t = c;
    if (c >= t + visible) t = c - visible + 1;
    if (t > count - visible) t = count - visible;
    if (t < 0) t = 0;
}

/* ---- AI Battle page ------------------------------------------------------------
 *
 * Two units, each one of the AC cards (Raven Card Builder images) in the
 * memory card's folder, or RANDOM. The choice reaches the game as
 * PSX_MOA_AI_UNIT1 / PSX_MOA_AI_UNIT2 (a card's path; empty = random), read by
 * games/master-of-arena/moa_ai_battle.c at VS START.
 */
static void set_ai_battle(bool on);
static void start_game(LauncherModel* m);

struct AiCard {
    std::string path;
    moa_card card;
    std::string label;                          /* AC name, else the file's name */
};
static std::vector<AiCard> g_ai_cards;
static std::string g_ai_folder;
static int g_ai_pick[2] = { -1, -1 };          /* index into g_ai_cards; -1 random */
static std::string g_ai_chosen[2];             /* paths, kept across rescans */
static bool g_ai_manual = false;               /* player 1 flies unit 1 */

static void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

/* The game reads cards from the folder that holds memory card 1. */
static std::string ai_card_folder(const LauncherModel* m) {
    const char* card = m->s.memcard_path[0];
    if (card && card[0]) {
        const std::filesystem::path p = std::filesystem::path(card).parent_path();
        if (!p.empty()) return p.string();
    }
    const char* home = std::getenv("PSX_MEMCARD_HOME");
    return home ? std::string(home) : std::string();
}

static void scan_ai_cards(const LauncherModel* m) {
    g_ai_cards.clear();
    g_ai_folder = ai_card_folder(m);
    std::error_code ec;
    if (g_ai_folder.empty() || !std::filesystem::is_directory(g_ai_folder, ec)) return;
    std::vector<std::string> paths;
    for (const auto& e : std::filesystem::directory_iterator(g_ai_folder, ec)) {
        if (!e.is_regular_file(ec)) continue;
        std::string ext = e.path().extension().string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".png") paths.push_back(e.path().string());
    }
    std::sort(paths.begin(), paths.end());
    for (const std::string& path : paths) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) continue;
        std::vector<uint8_t> data;
        if (std::fseek(f, 0, SEEK_END) == 0) {
            const long n = std::ftell(f);
            if (n > 0 && n < 32L * 1024 * 1024 && std::fseek(f, 0, SEEK_SET) == 0) {
                data.resize((size_t)n);
                if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
            }
        }
        std::fclose(f);
        AiCard c;
        c.path = path;
        if (!data.empty() && moa_card_decode(data.data(), data.size(), &c.card)) {
            c.label = upper(c.card.ac[0] ? c.card.ac
                                         : std::filesystem::path(path).stem().string().c_str());
            g_ai_cards.push_back(std::move(c));
        }
    }
    for (int u = 0; u < 2; ++u) {
        g_ai_pick[u] = -1;
        for (size_t i = 0; i < g_ai_cards.size(); ++i)
            if (g_ai_cards[i].path == g_ai_chosen[u]) g_ai_pick[u] = (int)i;
    }
}

/* Every scenario save on memory card 1 becomes a card in the card folder. */
static void export_ai_cards(LauncherModel* m) {
    const std::string folder = ai_card_folder(m);
    const char* card = m->s.memcard_path[0];
    const std::string mcd = (card && card[0]) ? std::string(card)
                                              : (std::filesystem::path(folder) / "card1.mcd").string();
    if (folder.empty()) { note("NO MEMORY CARD FOLDER YET"); return; }
    const int n = moa_card_export_memcard(mcd.c_str(), folder.c_str());
    char buf[96];
    if (n < 0) std::snprintf(buf, sizeof buf, "MEMORY CARD 1 COULD NOT BE READ");
    else if (n == 0) std::snprintf(buf, sizeof buf, "NO NEW CARDS (NO SCENARIO SAVE ON CARD 1?)");
    else std::snprintf(buf, sizeof buf, "EXPORTED %d CARD%s", n, n == 1 ? "" : "S");
    scan_ai_cards(m);
    note(buf);
}

static void draw_emblem(const Gfx& g, float x, float y, float size, const moa_card& c);
#include "moa_menu_ai.inc"

static void open_ai_page(LauncherModel* m) {
    scan_ai_cards(m);
    g_state.ai_page = true;
    g_ai_sub = AI_SUB_SETUP;
    g_state.cursor[PAGE_AI] = 0;
    g_state.note[0] = '\0';
}

static void step_ai_pick(int unit, int dir) {
    const int n = (int)g_ai_cards.size();
    int& p = g_ai_pick[unit];
    p += dir;                                   /* -1 .. n-1, wrapping */
    if (p < -1) p = n - 1;
    if (p >= n) p = -1;
    g_ai_chosen[unit] = p >= 0 ? g_ai_cards[(size_t)p].path : std::string();
}

/* The card's 64x64 game emblem, one rectangle per pixel. */
static void draw_emblem(const Gfx& g, float x, float y, float size, const moa_card& c) {
    const float px = size / 64.0f;
    for (int j = 0; j < 64; ++j)
        for (int i = 0; i < 64; ++i) {
            const uint16_t v = c.palette[c.emblem[j * 64 + i] & 15];
            const ImU32 col = IM_COL32((v & 31) * 255 / 31, ((v >> 5) & 31) * 255 / 31,
                                       ((v >> 10) & 31) * 255 / 31, 255);
            g.dl->AddRectFilled(at(g, x + i * px, y + j * px),
                                at(g, x + (i + 1) * px, y + (j + 1) * px), col);
        }
    g.dl->AddRect(at(g, x, y), at(g, x + size, y + size), kLine);
}

static void draw_unit(const Gfx& g, float x, int unit) {
    char cap[16];
    std::snprintf(cap, sizeof cap, "UNIT %d", unit + 1);
    panel(g, x, 76.0f, 145.0f, 214.0f, cap);
    const int p = g_ai_pick[unit];
    if (p < 0) {
        g.dl->AddRect(at(g, x + 16.0f, 92.0f), at(g, x + 129.0f, 205.0f), kLine);
        const float tw = text_width(g, 30.0f, "?");
        text(g, x + 72.5f - tw * 0.5f, 132.0f, 30.0f, kDim, "?");
        text(g, x + 16.0f, 216.0f, 13.0f, kHi, "RANDOM");
        text(g, x + 16.0f, 236.0f, 10.0f, kDim, "A CARD FROM THE FOLDER,", 0.0f);
        text(g, x + 16.0f, 250.0f, 10.0f, kDim, "OR AN ARENA OPPONENT", 0.0f);
        text(g, x + 16.0f, 268.0f, 10.0f, kDim, "(BALANCED AI)", 0.0f);
        return;
    }
    const moa_card& c = g_ai_cards[(size_t)p].card;
    draw_emblem(g, x + 16.0f, 92.0f, 113.0f, c);
    text(g, x + 16.0f, 216.0f, 13.0f, kBright, g_ai_cards[(size_t)p].label.c_str());
    text(g, x + 16.0f, 236.0f, 10.0f, kDim, "PILOT");
    text(g, x + 16.0f, 250.0f, 11.0f, kText, upper(c.pilot).c_str());
    text(g, x + 16.0f, 268.0f, 10.0f, kHi, ("AI  " + card_ai_label(c)).c_str());
}

static void draw_ai_page(LauncherModel* m, const Gfx& g, const Input& in) {
    if (g_ai_sub == AI_SUB_CARDS) { draw_ai_cards(m, g, in); return; }
    if (g_ai_sub == AI_SUB_EDIT)  { draw_ai_edit(m, g, in); return; }
    if (g_ai_sub == AI_SUB_ADV)   { draw_ai_adv(g, in); return; }
    if (g_ai_sub == AI_SUB_BUILD) { draw_ai_build(m, g, in); return; }
    if (g_ai_sub == AI_SUB_PICK)  { draw_ai_pick(g, in); return; }
    if (g_ai_sub == AI_SUB_OPTS)  { draw_ai_opts(g, in); return; }
    if (g_ai_sub == AI_SUB_PAINT) { draw_ai_paint(g, in); return; }
    if (g_ai_sub == AI_SUB_EMBLEM){ draw_ai_emblem(m, g, in); return; }
    if (g_ai_sub == AI_SUB_BUILD) { draw_ai_build(m, g, in); return; }
    if (g_ai_sub == AI_SUB_PICK)  { draw_ai_pick(g, in); return; }
    if (g_ai_sub == AI_SUB_OPTS)  { draw_ai_opts(g, in); return; }
    if (g_ai_sub == AI_SUB_PAINT) { draw_ai_paint(g, in); return; }
    if (g_ai_sub == AI_SUB_EMBLEM){ draw_ai_emblem(m, g, in); return; }
    enum { AI_START = 0, AI_UNIT1, AI_UNIT2, AI_PILOT1, AI_WORKSHOP, AI_EXPORT, AI_ROWS };
    move_cursor(PAGE_AI, AI_ROWS, in, AI_ROWS);
    int& cur = g_state.cursor[PAGE_AI];

    header(g, "AI BATTLE", "COMPUTER VERSUS COMPUTER");
    panel(g, 24.0f, 76.0f, 270.0f, 214.0f, "SETUP");
    int activate = -1;
    const char* labels[AI_ROWS] = { "START BATTLE", "UNIT 1", "UNIT 2", "UNIT 1 PILOT",
                                    "CARD WORKSHOP", "EXPORT CARDS FROM MEMORY CARD" };
    for (int i = 0; i < AI_ROWS; ++i) {
        std::string value;
        if (i == AI_UNIT1 || i == AI_UNIT2) {
            const int p = g_ai_pick[i - AI_UNIT1];
            value = p < 0 ? std::string("RANDOM") : g_ai_cards[(size_t)p].label;
        } else if (i == AI_PILOT1) {
            value = g_ai_manual ? "YOU" : "COMPUTER";
        }
        bool hovered = false;
        if (row(g, 25.0f, 90.0f + kRowH * i, 268.0f, i == cur, true, labels[i],
                value.c_str(), false, false, &hovered))
            activate = i;
        if (hovered && (ImGui::GetIO().MouseDelta.x != 0.0f ||
                        ImGui::GetIO().MouseDelta.y != 0.0f))
            cur = i;
    }
    draw_unit(g, 310.0f, 0);
    draw_unit(g, 471.0f, 1);

    panel(g, 24.0f, 306.0f, 592.0f, 110.0f, "CARDS");
    {
        char buf[300];
        std::snprintf(buf, sizeof buf, "%d CARD%s FOUND", (int)g_ai_cards.size(),
                      g_ai_cards.size() == 1 ? "" : "S");
        text(g, 38.0f, 322.0f, 11.0f, g_ai_cards.empty() ? kWarn : kHi, buf);
        text(g, 38.0f, 342.0f, 10.0f, kText,
             "Cards are AC images saved by the Raven Card Builder. Each card carries its "
             "AI; set it in the CARD WORKSHOP. Cards live in the memory card folder:", 560.0f);
        text(g, 38.0f, 384.0f, 10.0f, kDim,
             g_ai_folder.empty() ? "(no memory card folder yet)" : g_ai_folder.c_str(), 560.0f);
    }
    footer(g, "A  SELECT      LEFT / RIGHT  CHANGE      B  BACK      START  BATTLE");

    if (in.back) { g_state.ai_page = false; return; }
    if ((cur == AI_UNIT1 || cur == AI_UNIT2) && (in.left || in.right))
        step_ai_pick(cur - AI_UNIT1, in.left ? -1 : 1);
    if (cur == AI_PILOT1 && (in.left || in.right)) g_ai_manual = !g_ai_manual;
    if (in.confirm) activate = cur;
    if (in.start) activate = AI_START;
    if (activate < 0) return;
    cur = activate;
    if (activate == AI_EXPORT) { export_ai_cards(m); return; }
    if (activate == AI_PILOT1) { g_ai_manual = !g_ai_manual; return; }
    if (activate == AI_WORKSHOP) {
        g_ai_sub = AI_SUB_CARDS;
        g_ws_action = -1;
        g_confirm_delete = false;
        return;
    }
    if (activate != AI_START) { step_ai_pick(activate - AI_UNIT1, 1); return; }
    if (g_ai_pick[0] >= 0 && g_ai_pick[0] == g_ai_pick[1]) {
        note("PICK TWO DIFFERENT CARDS");
        return;
    }
    for (int u = 0; u < 2; ++u)
        set_env(u ? "PSX_MOA_AI_UNIT2" : "PSX_MOA_AI_UNIT1",
                g_ai_pick[u] >= 0 ? g_ai_cards[(size_t)g_ai_pick[u]].path.c_str() : "");
    set_env("PSX_MOA_AI_P1", g_ai_manual ? "1" : "0");
    set_ai_battle(true);
    g_state.ai_page = false;
    start_game(m);
}

/* ---- extras page ------------------------------------------------------------- */

/* The arena package built from the player's own discs (games/master-of-arena/
 * moa_phantasma.c). */
static std::string new_mods_dir();
static const char* const kPhantasmaPackage = "community.moa.phantasma-arena";
static const char* const kPhantasmaVersion = "2.0.0";   /* what moa_phantasma_build() writes */
static std::string g_pp_found;     /* the Phantasma disc found beside the game disc */
static std::string g_pp_status;    /* the last result, shown on the page */
static bool        g_pp_failed = false;

static std::string game_disc(const LauncherModel* m) {
    const char* p = launcher_model_effective_rom_path(m);
    if (!p || !p[0]) p = launcher_model_rom_path(m);
    return p ? p : "";
}

/* A Project Phantasma disc image in the folder of the Master of Arena disc. */
static std::string find_phantasma_disc(const LauncherModel* m) {
    namespace fs = std::filesystem;
    const std::string moa = game_disc(m);
    if (moa.empty()) return "";
    std::error_code ec;
    const fs::path dir = fs::u8path(moa).parent_path();
    std::vector<std::string> cues, images;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        const std::string path = it->path().u8string();
        if (path == moa) continue;
        const std::string ext = upper(it->path().extension().u8string().c_str());
        if (ext == ".CUE") cues.push_back(path);
        else if (ext == ".BIN" || ext == ".ISO") images.push_back(path);
    }
    std::sort(cues.begin(), cues.end());
    std::sort(images.begin(), images.end());
    for (const std::string& p : cues)   if (moa_phantasma_is_disc(p.c_str())) return p;
    for (const std::string& p : images) if (moa_phantasma_is_disc(p.c_str())) return p;
    return "";
}

/* Installed with the missions (package 2.0): an arena-only 1.x install is
 * rebuilt so the missions come along. */
static bool phantasma_installed(const LauncherModel* m) {
    const RecompLauncherCModProvider* mods = m->mods;
    if (!mods || !mods->feature_count || !mods->feature_get) return false;
    const int n = mods->feature_count(mods->ctx);
    for (int i = 0; i < n; ++i) {
        RecompLauncherCModFeature f;
        std::memset(&f, 0, sizeof f);
        if (mods->feature_get(mods->ctx, i, &f) && std::strcmp(f.package_id, kPhantasmaPackage) == 0 &&
            std::strcmp(f.id, "missions") == 0)
            return true;
    }
    return false;
}

static void open_extras_page(LauncherModel* m) {
    g_state.extras_page = true;
    g_state.cursor[PAGE_EXTRAS] = 0;
    g_state.note[0] = '\0';
    g_pp_found = find_phantasma_disc(m);
    g_pp_status.clear();
    g_pp_failed = false;
}

/* Switch off and remove every installed version of the package (an older
 * arena-only 1.x left beside 2.0 keeps the selection on 1.x, so the missions
 * never switch on). */
static bool remove_phantasma(LauncherModel* m) {
    const RecompLauncherCModProvider* mods = m->mods;
    if (!mods || !mods->remove_package || !mods->version_count || !mods->version_get) return false;
    if (mods->feature_count && mods->feature_get && mods->feature_enable) {
        const int n = mods->feature_count(mods->ctx);
        std::vector<std::string> ids;
        for (int i = 0; i < n; ++i) {
            RecompLauncherCModFeature f;
            std::memset(&f, 0, sizeof f);
            if (mods->feature_get(mods->ctx, i, &f) && std::strcmp(f.package_id, kPhantasmaPackage) == 0)
                ids.push_back(f.id);
        }
        for (const std::string& id : ids) mods->feature_enable(mods->ctx, kPhantasmaPackage, id.c_str(), 0);
    }
    if (mods->set_enabled) mods->set_enabled(mods->ctx, kPhantasmaPackage, 0);
    std::vector<std::string> versions;
    const int vn = mods->version_count(mods->ctx, kPhantasmaPackage);
    for (int i = 0; i < vn; ++i) {
        RecompLauncherCModVersion v;
        std::memset(&v, 0, sizeof v);
        if (mods->version_get(mods->ctx, kPhantasmaPackage, i, &v)) versions.push_back(v.version);
    }
    bool ok = true;
    for (const std::string& v : versions)
        if (!mods->remove_package(mods->ctx, kPhantasmaPackage, v.c_str())) ok = false;
    return ok;
}

/* Build the arena and missions from the two discs, install them and switch
 * them on. `reinstall` removes whatever is installed first. */
static void install_phantasma(LauncherModel* m, const std::string& phantasma, bool reinstall) {
    namespace fs = std::filesystem;
    const RecompLauncherCModProvider* mods = m->mods;
    const std::string moa = game_disc(m);
    g_pp_failed = true;
    if (!mods || !mods->install_archive) { g_pp_status = "MODS ARE NOT AVAILABLE IN THIS BUILD"; return; }
    if (moa.empty()) { g_pp_status = "SELECT YOUR MASTER OF ARENA DISC FIRST"; return; }
    std::error_code ec;
    const fs::path dir = fs::u8path(new_mods_dir());
    fs::create_directories(dir, ec);
    const fs::path file = dir / "MoA-Phantasma-2.0.0.psxmod";
    char error[240] = {0};
    if (!moa_phantasma_build(moa.c_str(), phantasma.c_str(), file.u8string().c_str(),
                             error, sizeof error)) {
        std::error_code rec;
        fs::remove(file, rec);
        g_pp_status = upper(error[0] ? error : "PROJECT PHANTASMA COULD NOT BE BUILT.");
        return;
    }
    /* Any version already installed goes first: a reinstall always, and an
     * older version (arena only) on a plain install. */
    if ((reinstall || (mods->version_count && mods->version_count(mods->ctx, kPhantasmaPackage) > 0)) &&
        !remove_phantasma(m)) {
        std::error_code rec;
        fs::remove(file, rec);
        mod_note_error(m);
        g_pp_status = "THE INSTALLED CONTENT COULD NOT BE REMOVED: " + upper(m->mod_status);
        return;
    }
    const bool installed = mods->install_archive(mods->ctx, file.u8string().c_str()) != 0;
    if (!installed) mod_note_error(m);
    /* The package holds data from the player's discs: it stays inside the
     * game's own mods folder and is not left lying in mods/new. */
    std::error_code rec;
    fs::remove(file, rec);
    if (!installed) { g_pp_status = upper(m->mod_status); return; }
    if (mods->select_version) mods->select_version(mods->ctx, kPhantasmaPackage, kPhantasmaVersion);
    if (mods->set_enabled) mods->set_enabled(mods->ctx, kPhantasmaPackage, 1);
    if (mods->feature_count && mods->feature_get && mods->feature_enable) {
        const int n = mods->feature_count(mods->ctx);
        std::vector<std::string> ids;
        for (int i = 0; i < n; ++i) {
            RecompLauncherCModFeature f;
            std::memset(&f, 0, sizeof f);
            if (mods->feature_get(mods->ctx, i, &f) && std::strcmp(f.package_id, kPhantasmaPackage) == 0)
                ids.push_back(f.id);
        }
        for (const std::string& id : ids) mods->feature_enable(mods->ctx, kPhantasmaPackage, id.c_str(), 1);
    }
    g_pp_failed = false;
    g_pp_status = reinstall ? "REINSTALLED AND SWITCHED ON: TENTH EX ARENA AND PP: MISSIONS."
                            : "INSTALLED AND SWITCHED ON: TENTH EX ARENA AND PP: MISSIONS.";
}

static void choose_phantasma(LauncherModel* m, bool reinstall) {
    if (!reinstall && phantasma_installed(m)) {
        g_pp_failed = false;
        g_pp_status = "ALREADY INSTALLED. USE REINSTALL TO BUILD IT AGAIN.";
        return;
    }
    if (!g_pp_found.empty()) {   /* beside the game disc: nothing to pick */
        install_phantasma(m, g_pp_found, reinstall);
        return;
    }
    ui_pick_file(m, "Select the Project Phantasma disc", {"*.cue", "*.bin", "*.iso"},
                 "Disc image", [m, reinstall](const char* path) {
        if (!path) return;
        if (!moa_phantasma_is_disc(path)) {
            g_pp_failed = true;
            g_pp_status = "THAT IS NOT THE PROJECT PHANTASMA (USA) DISC";
            return;
        }
        install_phantasma(m, path, reinstall);
    });
}

static void draw_extras_page(LauncherModel* m, const Gfx& g, const Input& in) {
    move_cursor(PAGE_EXTRAS, 2, in, 2);
    int& cur = g_state.cursor[PAGE_EXTRAS];
    header(g, "EXTRAS", "BUILT FROM DISCS YOU OWN");
    panel(g, 24.0f, 76.0f, 592.0f, 70.0f, "SELECT");
    bool hovered0 = false, hovered1 = false;
    const bool installed = phantasma_installed(m);
    const bool clicked0 = row(g, 25.0f, 90.0f, 590.0f, cur == 0,
                              true, "INSTALL DATA FROM PROJECT PHANTASMA",
                              installed ? "INSTALLED" : "", false, false, &hovered0);
    const bool clicked1 = row(g, 25.0f, 90.0f + kRowH + 2.0f, 590.0f, cur == 1,
                              true, "REINSTALL ADDITIONAL CONTENT", "", false, false, &hovered1);
    if (hovered0) cur = 0;
    if (hovered1) cur = 1;

    panel(g, 24.0f, 152.0f, 592.0f, 264.0f, "PROJECT PHANTASMA");
    text(g, 38.0f, 168.0f, 11.0f, kText,
         "Adds the 49 arena opponents of Armored Core: Project Phantasma as a "
         "tenth Ex Arena, and its missions ('PP:', one at a time; 12 of 17 for now) at the top of "
         "the mission list. It is built here, on this computer, from your own "
         "Project Phantasma (USA) disc. Nothing from the disc is copied anywhere "
         "else.", 560.0f);
    text(g, 38.0f, 256.0f, 11.0f, kDim, "PHANTASMA DISC");
    if (installed) {
        text(g, 170.0f, 256.0f, 11.0f, kOn, "NOT NEEDED: ALREADY INSTALLED");
    } else if (!g_pp_found.empty()) {
        text(g, 170.0f, 256.0f, 11.0f, kOn, "FOUND NEXT TO YOUR GAME DISC");
        text(g, 38.0f, 274.0f, 10.0f, kDim,
             std::filesystem::u8path(g_pp_found).filename().u8string().c_str(), 560.0f);
    } else {
        text(g, 170.0f, 256.0f, 11.0f, kWarn, "NOT FOUND");
        text(g, 38.0f, 274.0f, 10.0f, kText,
             "Put the Project Phantasma disc image (.cue and .bin) in the same "
             "folder as your Master of Arena disc and reopen this page, or "
             "choose the file yourself: you will be asked when you select the "
             "option. The file window belongs to the system and needs a mouse "
             "or keyboard.", 560.0f);
    }
    if (!g_pp_status.empty())
        text(g, 38.0f, 340.0f, 11.0f, g_pp_failed ? kWarn : kHi, g_pp_status.c_str(), 560.0f);
    footer(g, "A / ENTER  SELECT      B / ESC  BACK");

    if (in.back) { g_state.extras_page = false; return; }
    if (clicked0 || (in.confirm && cur == 0)) choose_phantasma(m, false);
    else if (clicked1 || (in.confirm && cur == 1)) choose_phantasma(m, true);
}

/* ---- Updates page ----------------------------------------------------------------
 *
 * Opt-in: nothing is looked up or downloaded until a button here is pressed
 * (games/master-of-arena/moa_update.cpp does the work on its own thread).
 * Only the program's own files are replaced; memory cards, mods, disc images
 * and settings are left alone.
 */
static bool update_live(const moa_update::Snapshot& s) {
    using moa_update::State;
    return s.state == State::Available || s.state == State::Downloading ||
           s.state == State::Applying;
}

static void open_updates_page() {
    g_state.updates_page = true;
    g_state.cursor[PAGE_UPDATES] = 0;
    g_state.note[0] = '\0';
}

static void draw_updates_page(const Gfx& g, const Input& in) {
    using moa_update::State;
    const moa_update::Snapshot s = moa_update::snapshot();
    const bool busy = s.state == State::Checking || s.state == State::Downloading ||
                      s.state == State::Applying;
    std::string why;
    const bool can = moa_update::supported(&why);
    const bool can_apply = can && s.state == State::Available;

    move_cursor(PAGE_UPDATES, 2, in, 2);
    int& cur = g_state.cursor[PAGE_UPDATES];
    header(g, "UPDATES", "NOTHING IS DOWNLOADED UNTIL YOU ASK");
    panel(g, 24.0f, 76.0f, 592.0f, 64.0f, "SELECT");
    bool hov[2] = { false, false };
    const bool click0 = row(g, 25.0f, 90.0f, 590.0f, cur == 0, !busy,
                            "CHECK FOR UPDATE", "", false, false, &hov[0]);
    const bool click1 = row(g, 25.0f, 90.0f + kRowH, 590.0f, cur == 1, can_apply && !busy,
                            "DOWNLOAD AND APPLY UPDATE", "", false, false, &hov[1]);
    for (int i = 0; i < 2; ++i)
        if (hov[i] && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f))
            cur = i;

    panel(g, 24.0f, 156.0f, 592.0f, 260.0f, "VERSION");
    text(g, 38.0f, 172.0f, 11.0f, kDim, "INSTALLED");
    text(g, 170.0f, 172.0f, 11.0f, kText, s.current == "dev" ? "DEVELOPMENT BUILD" : s.current.c_str());
    text(g, 38.0f, 191.0f, 11.0f, kDim, "NEWEST");
    text(g, 170.0f, 191.0f, 11.0f, s.latest.empty() ? kDim : kText,
         s.latest.empty() ? "NOT CHECKED YET" : s.latest.c_str());
    if (!s.message.empty()) {
        const ImU32 c = s.state == State::Failed ? kWarn
                      : (s.state == State::Available) ? kHi
                      : (s.state == State::UpToDate || s.state == State::Done) ? kOn : kText;
        text(g, 38.0f, 214.0f, 11.0f, c, s.message.c_str(), 560.0f);
    }
    if (s.state == State::Downloading) {
        const float w = 540.0f;
        const float f = s.total ? std::min(1.0f, (float)((double)s.done / (double)s.total)) : 0.0f;
        g.dl->AddRect(at(g, 38.0f, 236.0f), at(g, 38.0f + w, 248.0f), kLine);
        g.dl->AddRectFilled(at(g, 39.0f, 237.0f), at(g, 39.0f + (w - 2.0f) * f, 247.0f), kHi);
        char b[80];
        std::snprintf(b, sizeof b, "%.1f / %.1f MB", s.done / 1048576.0, s.total / 1048576.0);
        text(g, 38.0f, 253.0f, 10.0f, kDim, b);
    } else if (s.state == State::Applying) {
        text(g, 38.0f, 236.0f, 10.0f, kWarn, "DO NOT CLOSE THE GAME WHILE THIS RUNS.");
    }
    if (!can) text(g, 38.0f, 282.0f, 11.0f, kWarn, why.c_str(), 560.0f);
    text(g, 38.0f, 308.0f, 11.0f, kText,
         "An update replaces only the game's own program files. Your memory "
         "cards, installed mods, disc images and settings are not touched. The "
         "new version starts the next time you open the game. Checking and "
         "downloading use the internet and go to GitHub only when you press a "
         "button here.", 560.0f);
    footer(g, "A / ENTER  SELECT      B / ESC  BACK");

    if (in.back) { g_state.updates_page = false; return; }
    if (busy) return;
    if (click0) cur = 0;
    if (click1) cur = 1;
    if (in.confirm || click0 || click1) {
        if (cur == 0) moa_update::check();
        else if (can_apply) moa_update::download_and_apply();
    }
}

/* ---- main page ------------------------------------------------------------- */

enum MainEntry { MAIN_START = 0, MAIN_NETWORK, MAIN_AI_BATTLE, MAIN_EXTRAS, MAIN_SETTINGS, MAIN_MODS, MAIN_UPDATES, MAIN_EXIT };

static int enabled_mod_count(const LauncherModel* m, int* total_out) {
    int on = 0, total = 0;
    const RecompLauncherCModProvider* mods = m->mods;
    if (mods && mods->feature_count && mods->feature_get) {
        const int n = mods->feature_count(mods->ctx);
        for (int i = 0; i < n; ++i) {
            RecompLauncherCModFeature f;
            std::memset(&f, 0, sizeof f);
            if (!mods->feature_get(mods->ctx, i, &f)) continue;
            if (f.hidden && !f.enabled) continue;
            ++total;
            if (f.enabled) ++on;
        }
    }
    if (total_out) *total_out = total;
    return on;
}

static void start_game(LauncherModel* m) {
    /* The stock PLAY button's decisions, in the same order. */
    if (launcher_model_bios_blocks_play(m)) {
        launcher_model_bios_play_prompt(m);
    } else if (!launcher_model_can_launch(m)) {
        if (m->setup_wizard_supported) m->setup_wizard_open = true;
        else note("SELECT A DISC IMAGE FIRST");
    } else if (!launcher_model_prepare_rom_patch(m)) {
        launcher_model_set_view(m, LNG_VIEW_MODS);
    } else {
        request_mod_commit_launch(m);
    }
}

/* AI Battle is a launch option the game reads (games/master-of-arena/
 * moa_ai_battle.c): the versus menu then fields two computer-flown ACs. */
static void set_ai_battle(bool on) {
#ifdef _WIN32
    _putenv_s("PSX_MOA_AI_BATTLE", on ? "1" : "0");
#else
    setenv("PSX_MOA_AI_BATTLE", on ? "1" : "0", 1);
#endif
}

static void draw_main(LauncherModel* m, const Gfx& g, const Input& in) {
    struct Entry { MainEntry id; const char* label; const char* info; };
    Entry entries[8];
    int count = 0;
    entries[count++] = { MAIN_START, m->in_session ? "RESUME GAME" : "START GAME",
        m->in_session ? "Return to the game in progress."
                      : "Start Armored Core: Master of Arena with the settings "
                        "and mods chosen here." };
    if (m->netplay_supported && !m->in_session)
        entries[count++] = { MAIN_NETWORK, "NETWORK PLAY",
            "Versus over LAN or online, two players. The host is player 1; "
            "the guest is player 2 and brings their own memory card." };
    if (!m->in_session)
        entries[count++] = { MAIN_AI_BATTLE, "AI BATTLE",
            "Watch two computer-flown ACs fight. Choose the two units from the "
            "AC cards in your memory card folder." };
    if (!m->in_session && m->mods)
        entries[count++] = { MAIN_EXTRAS, "EXTRAS",
            "Extra content built from discs you own, such as the Project "
            "Phantasma arena." };
    entries[count++] = { MAIN_SETTINGS, "SETTINGS",
        "Display, sound and controls." };
    if (m->mods)
        entries[count++] = { MAIN_MODS, "MODS",
            "Switch installed mods on or off, and install new ones." };
    if (!m->in_session)
        entries[count++] = { MAIN_UPDATES, "UPDATES",
            "Look for a newer version and install it when you choose. Nothing "
            "happens by itself, and your saves, mods and discs are not touched." };
    entries[count++] = { MAIN_EXIT, m->in_session ? "QUIT GAME" : "EXIT",
        m->in_session ? "Close the game." : "Close the launcher." };

    move_cursor(PAGE_MAIN, count, in, count);
    int& cur = g_state.cursor[PAGE_MAIN];

    header(g, "MASTER OF ARENA", "SYSTEM MENU");
    panel(g, 24.0f, 76.0f, 270.0f, 340.0f, "SELECT");
    int activate = -1;
    const moa_update::Snapshot upd = moa_update::snapshot();
    for (int i = 0; i < count; ++i) {
        bool hovered = false;
        if (row(g, 25.0f, 90.0f + kRowH * i, 268.0f, i == cur, true,
                entries[i].label,
                entries[i].id == MAIN_UPDATES && update_live(upd) ? "NEW" : nullptr,
                false, false, &hovered))
            activate = i;
        if (hovered && (ImGui::GetIO().MouseDelta.x != 0.0f ||
                        ImGui::GetIO().MouseDelta.y != 0.0f))
            cur = i;
    }
    if (in.confirm) activate = cur;
    if (in.start) activate = 0;

    /* Status. */
    panel(g, 310.0f, 76.0f, 306.0f, 150.0f, "STATUS");
    {
        float y = 92.0f;
        char buf[160];
        const bool ready = launcher_model_can_launch(m);
        text(g, 324.0f, y, 11.0f, kDim, "DISC");
        text(g, 424.0f, y, 11.0f, ready ? kOn : kWarn, ready ? "READY" : "NOT FOUND");
        y += 19.0f;
        text(g, 324.0f, y, 11.0f, kDim, "CONTROLLER");
        std::snprintf(buf, sizeof buf, "%s",
                      upper(launcher_model_player_src_label(m, 0)).c_str());
        text(g, 424.0f, y, 11.0f, kText, buf);
        y += 19.0f;
        text(g, 324.0f, y, 11.0f, kDim, "MEMORY CARD");
        std::snprintf(buf, sizeof buf, "%u / 15 BLOCKS",
                      (unsigned)launcher_model_memcard_blocks_used(m, 0));
        text(g, 424.0f, y, 11.0f, kText, buf);
        y += 19.0f;
        if (m->mods) {
            int total = 0;
            const int on = enabled_mod_count(m, &total);
            text(g, 324.0f, y, 11.0f, kDim, "MODS");
            std::snprintf(buf, sizeof buf, "%d ACTIVE OF %d", on, total);
            text(g, 424.0f, y, 11.0f, kText, buf);
            y += 19.0f;
        }
        text(g, 324.0f, y, 11.0f, kDim, "DISPLAY");
        std::snprintf(buf, sizeof buf, "%s  %s",
                      upper(launcher_model_aspect_label(m)).c_str(),
                      upper(launcher_model_fullscreen_label(m)).c_str());
        text(g, 424.0f, y, 11.0f, kText, buf);
        y += 19.0f;
        if (update_live(upd)) {
            text(g, 324.0f, y, 11.0f, kDim, "UPDATE");
            std::snprintf(buf, sizeof buf, "AVAILABLE  v%s", upd.latest.c_str());
            text(g, 424.0f, y, 11.0f, kHi, buf);
        } else if (upd.state == moa_update::State::Done) {
            text(g, 324.0f, y, 11.0f, kDim, "UPDATE");
            text(g, 424.0f, y, 11.0f, kOn, "INSTALLED. RESTART");
        }
    }

    panel(g, 310.0f, 242.0f, 306.0f, 174.0f, "INFO");
    text(g, 324.0f, 258.0f, 11.0f, kHi, entries[cur].label);
    text(g, 324.0f, 280.0f, 11.0f, kText, entries[cur].info, 278.0f);

    footer(g, "A / ENTER  SELECT      D-PAD  MOVE      START  START GAME");

    if (activate < 0) return;
    cur = activate;
    switch (entries[activate].id) {
        case MAIN_START:    if (!m->in_session) set_ai_battle(false);
                            start_game(m); break;
        case MAIN_AI_BATTLE: open_ai_page(m); break;
        case MAIN_EXTRAS:   open_extras_page(m); break;
        case MAIN_UPDATES:  open_updates_page(); break;
        case MAIN_NETWORK:  launcher_model_set_view(m, LNG_VIEW_NETPLAY_MODE); break;
        case MAIN_SETTINGS: launcher_model_set_view(m, LNG_VIEW_SETTINGS); break;
        case MAIN_MODS:     launcher_model_set_view(m, LNG_VIEW_MODS); break;
        case MAIN_EXIT:     m->action = LNG_ACTION_QUIT; break;
    }
}

/* ---- settings page ---------------------------------------------------------- */

struct Setting {
    const char* label;
    const char* detail;
    bool        (*offered)(LauncherModel*);
    const char* (*value)(LauncherModel*);
    /* dir: 0 confirm, -1 left, +1 right. */
    void        (*step)(LauncherModel*, int dir);
};

static bool always(LauncherModel*) { return true; }

static const char* volume_value(LauncherModel* m) {
    static char buf[16];
    std::snprintf(buf, sizeof buf, "%d", m->s.volume);
    return buf;
}

static void internal_resolution_step(LauncherModel* m, int dir) {
    const int n = launcher_model_internal_resolution_count(m);
    if (n <= 0) return;
    int i = launcher_model_internal_resolution_index(m) + (dir < 0 ? -1 : 1);
    if (i < 0) i = n - 1;
    if (i >= n) i = 0;
    launcher_model_set_internal_resolution(m, i);
}

static const Setting kSettings[] = {
    { "ASPECT", "The shape of the picture. 4:3 is the original; wider shapes "
                "show more of the battlefield.",
      always, [](LauncherModel* m) { return launcher_model_aspect_label(m); },
      [](LauncherModel* m, int) { launcher_model_cycle_aspect(m); } },
    { "FULLSCREEN", "Off, borderless or exclusive fullscreen.",
      always, [](LauncherModel* m) { return launcher_model_fullscreen_label(m); },
      [](LauncherModel* m, int) { launcher_model_cycle_fullscreen(m); } },
    { "WINDOW SIZE", "Size of the window when fullscreen is off.",
      always, [](LauncherModel* m) { return launcher_model_window_size_label(m); },
      [](LauncherModel* m, int) { launcher_model_cycle_window_size(m); } },
    { "INTERNAL RESOLUTION", "How finely the 3D picture is drawn. Higher is "
                             "sharper and needs a faster graphics card.",
      [](LauncherModel* m) { return launcher_model_internal_resolution_offered(m) &&
                                    launcher_model_internal_resolution_count(m) > 0; },
      [](LauncherModel* m) { return launcher_model_internal_resolution_label(m); },
      internal_resolution_step },
    { "TEXTURE FILTER", "Smooths textures. Off keeps the original sharp pixels.",
      always, [](LauncherModel* m) { return launcher_model_texture_filter_label(m); },
      [](LauncherModel* m, int) { launcher_model_toggle_texture_filter(m); } },
    { "VSYNC", "Matches the picture to the display to avoid tearing.",
      always, [](LauncherModel* m) { return launcher_model_vsync_label(m); },
      [](LauncherModel* m, int) { launcher_model_cycle_vsync(m); } },
    { "VOLUME", "Overall sound level.",
      always, volume_value,
      [](LauncherModel* m, int dir) { launcher_model_volume_delta(m, dir < 0 ? -5 : 5); } },
    { "SKIP THIS MENU", "Start the game directly next time, without this menu.",
      always, [](LauncherModel* m) -> const char* { return m->s.skip_launcher ? "On" : "Off"; },
      [](LauncherModel* m, int) { launcher_model_request_skip_toggle(m); } },
    { "BUTTON LAYOUT", "Choose the controller and change which button does what.",
      always, [](LauncherModel*) -> const char* { return ">"; },
      [](LauncherModel* m, int dir) { if (dir >= 0) launcher_model_open_config(m, 0); } },
    { "ALL SETTINGS", "The full settings page: renderer, filters, hotkeys and "
                      "everything else. Needs the D-pad and more patience.",
      always, [](LauncherModel*) -> const char* { return ">"; },
      [](LauncherModel*, int dir) { if (dir >= 0) g_state.classic_settings = true; } },
    { "RESTORE DEFAULTS", "Put every setting back to how it was first installed.",
      [](LauncherModel* m) { return launcher_model_can_restore_defaults(m); },
      [](LauncherModel*) -> const char* { return ">"; },
      [](LauncherModel* m, int dir) { if (dir >= 0) launcher_model_request_restore_defaults(m); } },
};

static void draw_settings_page(LauncherModel* m, const Gfx& g, const Input& in) {
    const Setting* rows[sizeof kSettings / sizeof kSettings[0]];
    int count = 0;
    for (const Setting& s : kSettings)
        if (s.offered(m)) rows[count++] = &s;
    const int visible = 12;
    move_cursor(PAGE_SETTINGS, count, in, visible);
    int& cur = g_state.cursor[PAGE_SETTINGS];
    const int top = g_state.top[PAGE_SETTINGS];

    header(g, "SETTINGS", "OPTIONAL SYSTEMS");
    panel(g, 24.0f, 76.0f, 340.0f, 340.0f, "DISPLAY / SYSTEM");
    int clicked = -1;
    for (int i = top; i < count && i < top + visible; ++i) {
        const char* v = rows[i]->value(m);
        const bool on = same_word(v, "On") || same_word(v, "Enabled");
        const bool off = same_word(v, "Off") || same_word(v, "Disabled");
        std::string shown;
        if (!on && !off && v) {
            shown = upper(v);
            if (shown != ">") shown = "< " + shown + " >";
        }
        bool hovered = false;
        if (row(g, 25.0f, 90.0f + kRowH * (i - top), 338.0f, i == cur, true,
                rows[i]->label, shown.c_str(), on || off, on, &hovered))
            clicked = i;
        if (hovered && (ImGui::GetIO().MouseDelta.x != 0.0f ||
                        ImGui::GetIO().MouseDelta.y != 0.0f))
            cur = i;
    }

    panel(g, 380.0f, 76.0f, 236.0f, 340.0f, "DETAIL");
    if (count > 0) {
        text(g, 394.0f, 92.0f, 11.0f, kHi, rows[cur]->label);
        text(g, 394.0f, 114.0f, 11.0f, kText, rows[cur]->detail, 208.0f);
        char pos[32];
        std::snprintf(pos, sizeof pos, "ITEM %02d / %02d", cur + 1, count);
        text_right(g, 604.0f, 396.0f, 10.0f, kDim, pos);
    }
    footer(g, "A / ENTER  CHANGE      LEFT / RIGHT  ADJUST      B / ESC  BACK");

    if (clicked >= 0) { cur = clicked; rows[cur]->step(m, 0); return; }
    if (count > 0) {
        if (in.confirm)      rows[cur]->step(m, 0);
        else if (in.right)   rows[cur]->step(m, 1);
        else if (in.left)    rows[cur]->step(m, -1);
    }
    if (in.back) launcher_model_set_view(m, LNG_VIEW_DASHBOARD);
}

/* ---- mods page --------------------------------------------------------------- */

/* The drop folder for new mod packages: mods/new beside the game, or under
 * the mods folder a release names (PSX_MODS_DIR, see launcher/moa_shell.c). */
static std::string new_mods_dir() {
    const char* named = std::getenv("PSX_MODS_DIR");
    if (named && named[0]) {
        std::string dir(named);
        for (char& c : dir) if (c == '\\') c = '/';
        if (dir.back() != '/') dir.push_back('/');
        return dir + "new";
    }
    return asset("mods/new");
}

static const char* archive_extension(const LauncherModel* m) {
    return (m->mods && m->mods->archive_extension && m->mods->archive_extension[0])
               ? m->mods->archive_extension : ".psxmod";
}

static void scan_new_packages(const LauncherModel* m) {
    const double now = ImGui::GetTime();
    if (now < g_state.next_scan) return;
    g_state.next_scan = now + 1.0;
    g_state.new_packages.clear();
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::u8path(new_mods_dir());
    std::filesystem::create_directories(dir, ec);
    const std::string want = upper(archive_extension(m));
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        if (upper(it->path().extension().u8string().c_str()) != want) continue;
        g_state.new_packages.push_back(it->path().filename().u8string());
        if (g_state.new_packages.size() >= 16) break;
    }
    std::sort(g_state.new_packages.begin(), g_state.new_packages.end());
}

static void install_new_package(LauncherModel* m, const std::string& name) {
    const RecompLauncherCModProvider* mods = m->mods;
    const std::filesystem::path src =
        std::filesystem::u8path(new_mods_dir()) / std::filesystem::u8path(name);
    const std::string path = src.u8string();
    if (!mods || !mods->install_archive ||
        !mods->install_archive(mods->ctx, path.c_str())) {
        mod_note_error(m);
        note(m->mod_status);
        return;
    }
    /* Installed: it is the game's copy now. Park the file so it is not
     * offered again, without deleting what the player put there. */
    std::error_code ec;
    std::filesystem::path done = src;
    done += ".installed";
    std::filesystem::rename(src, done, ec);
    g_state.next_scan = 0.0;
    note("INSTALLED. SWITCH IT ON IN THE LIST.");
}

static void remove_feature_package(LauncherModel* m, const RecompLauncherCModFeature& f) {
    const RecompLauncherCModProvider* mods = m->mods;
    if (!mods || !mods->package_count || !mods->package_get || !mods->remove_package)
        return;
    const int n = mods->package_count(mods->ctx);
    for (int i = 0; i < n; ++i) {
        RecompLauncherCModPackage p;
        std::memset(&p, 0, sizeof p);
        if (!mods->package_get(mods->ctx, i, &p)) continue;
        if (std::strcmp(p.id, f.package_id) != 0) continue;
        if (!p.removable) { note("BUILT IN: CANNOT BE REMOVED"); return; }
        if (!mods->remove_package(mods->ctx, p.id, p.version)) {
            mod_note_error(m);
            note(m->mod_status);
        } else {
            note("REMOVED");
        }
        return;
    }
}

static void draw_mods_page(LauncherModel* m, const Gfx& g, const Input& in) {
    const RecompLauncherCModProvider* mods = m->mods;
    std::vector<RecompLauncherCModFeature> features;
    if (mods && mods->feature_count && mods->feature_get) {
        const int n = mods->feature_count(mods->ctx);
        for (int i = 0; i < n; ++i) {
            RecompLauncherCModFeature f;
            std::memset(&f, 0, sizeof f);
            if (!mods->feature_get(mods->ctx, i, &f)) continue;
            if (f.hidden && !f.enabled) continue;
            features.push_back(f);
        }
    }
    scan_new_packages(m);

    /* One cursor over both lists: features, then the ADD rows. */
    enum AddRow { ADD_BROWSE = 0, ADD_FOLDER, ADD_CLASSIC, ADD__COUNT };
    const int nf = (int)features.size();
    const int nn = (int)g_state.new_packages.size();
    const int count = nf + nn + ADD__COUNT;
    const int visible_features = 7;
    /* The feature list scrolls; the ADD rows are always on screen. */
    {
        int& c = g_state.cursor[PAGE_MODS];
        int& t = g_state.top[PAGE_MODS];
        if (in.up)   c = (c + count - 1) % count;
        if (in.down) c = (c + 1) % count;
        if (c < 0) c = 0;
        if (c >= count) c = count - 1;
        if (c < nf) {
            if (c < t) t = c;
            if (c >= t + visible_features) t = c - visible_features + 1;
        }
        if (t > nf - visible_features) t = nf - visible_features;
        if (t < 0) t = 0;
    }
    int& cur = g_state.cursor[PAGE_MODS];
    const int top = g_state.top[PAGE_MODS];

    header(g, "MODS", "OPTIONAL PARTS");
    panel(g, 24.0f, 76.0f, 340.0f, 206.0f, "INSTALLED");
    int clicked = -1;
    for (int i = top; i < nf && i < top + visible_features; ++i) {
        const RecompLauncherCModFeature& f = features[i];
        bool hovered = false;
        if (row(g, 25.0f, 86.0f + kRowH * (i - top), 338.0f, i == cur,
                !f.has_error, upper(f.name).c_str(), nullptr, true,
                f.enabled != 0, &hovered))
            clicked = i;
        if (hovered && (ImGui::GetIO().MouseDelta.x != 0.0f ||
                        ImGui::GetIO().MouseDelta.y != 0.0f))
            cur = i;
    }
    if (nf == 0)
        text(g, 55.0f, 96.0f, 12.0f, kDim, "NO MODS INSTALLED");
    if (nf > top + visible_features)
        text(g, 55.0f, 268.0f, 9.0f, kDim, "MORE BELOW");
    if (top > 0)
        text_right(g, 350.0f, 268.0f, 9.0f, kDim, "MORE ABOVE");

    const float add_h = 14.0f + kRowH * (nn + ADD__COUNT > 5 ? 5 : nn + ADD__COUNT);
    panel(g, 24.0f, 298.0f, 340.0f, 118.0f, "ADD");
    (void)add_h;
    /* Up to four rows fit; new packages first, then the fixed entries. */
    const int add_total = nn + ADD__COUNT;
    int add_top = 0;
    if (cur >= nf) {
        const int rel = cur - nf;
        if (rel >= 4) add_top = rel - 3;
    }
    for (int i = add_top; i < add_total && i < add_top + 4; ++i) {
        const int index = nf + i;
        std::string label;
        const char* value = ">";
        if (i < nn) { label = g_state.new_packages[i]; value = "INSTALL >"; }
        else if (i - nn == ADD_BROWSE)  label = "BROWSE FOR A FILE";
        else if (i - nn == ADD_FOLDER)  label = "OPEN MODS FOLDER";
        else                            label = "MOD OPTIONS (FULL PAGE)";
        bool hovered = false;
        if (row(g, 25.0f, 306.0f + kRowH * (i - add_top), 338.0f, index == cur,
                true, label.c_str(), value, false, false, &hovered))
            clicked = index;
        if (hovered && (ImGui::GetIO().MouseDelta.x != 0.0f ||
                        ImGui::GetIO().MouseDelta.y != 0.0f))
            cur = index;
    }

    panel(g, 380.0f, 76.0f, 236.0f, 340.0f, "DETAIL");
    if (cur < nf) {
        const RecompLauncherCModFeature& f = features[cur];
        text(g, 394.0f, 92.0f, 11.0f, kHi, upper(f.name).c_str(), 208.0f);
        text(g, 394.0f, 128.0f, 11.0f, kText, f.description, 208.0f);
        char buf[200];
        float y = 300.0f;
        if (f.package_name[0]) {
            text(g, 394.0f, y, 10.0f, kDim, "PACKAGE");
            text(g, 394.0f, y + 13.0f, 10.0f, kText, f.package_name, 208.0f);
            y += 34.0f;
        }
        std::snprintf(buf, sizeof buf, "VERSION  %s",
                      f.package_version[0] ? f.package_version : "-");
        text(g, 394.0f, y, 10.0f, kDim, buf);
        y += 16.0f;
        if (f.status[0])
            text(g, 394.0f, y, 10.0f, f.has_error ? kWarn : kDim, f.status, 208.0f);
    } else if (cur - nf < nn) {
        text(g, 394.0f, 92.0f, 11.0f, kHi, "INSTALL");
        text(g, 394.0f, 114.0f, 11.0f, kText,
             "A mod package found in the mods/new folder next to the game. "
             "Install copies it into the game and adds it to the list, "
             "switched off.", 208.0f);
    } else if (cur - nf - nn == ADD_BROWSE) {
        text(g, 394.0f, 92.0f, 11.0f, kHi, "BROWSE FOR A FILE");
        text(g, 394.0f, 114.0f, 11.0f, kText,
             "Pick a mod package anywhere on this computer. The file window "
             "belongs to the system and needs a mouse or keyboard.", 208.0f);
    } else if (cur - nf - nn == ADD_FOLDER) {
        text(g, 394.0f, 92.0f, 11.0f, kHi, "OPEN MODS FOLDER");
        text(g, 394.0f, 114.0f, 11.0f, kText,
             "Opens the mods/new folder. Mod packages placed there appear in "
             "this list, ready to install with the controller.", 208.0f);
    } else {
        text(g, 394.0f, 92.0f, 11.0f, kHi, "MOD OPTIONS");
        text(g, 394.0f, 114.0f, 11.0f, kText,
             "The full mods page, for mods that have settings of their own, "
             "version choices and search.", 208.0f);
    }
    {
        int total = 0;
        const int on = enabled_mod_count(m, &total);
        char pos[48];
        std::snprintf(pos, sizeof pos, "ACTIVE %02d / %02d   NEW %02d", on, total, nn);
        text_right(g, 604.0f, 396.0f, 10.0f, kDim, pos);
    }
    footer(g, "A / ENTER  TOGGLE OR INSTALL      X / DEL  REMOVE      B / ESC  BACK");

    const bool confirm = in.confirm || clicked >= 0;
    if (clicked >= 0) cur = clicked;
    if (confirm) {
        if (cur < nf) {
            const RecompLauncherCModFeature& f = features[cur];
            if (!mods->feature_enable ||
                !mods->feature_enable(mods->ctx, f.package_id, f.id, f.enabled ? 0 : 1)) {
                mod_note_error(m);
                note(m->mod_status);
            } else {
                m->mod_status[0] = '\0';
            }
        } else if (cur - nf < nn) {
            install_new_package(m, g_state.new_packages[cur - nf]);
        } else if (cur - nf - nn == ADD_BROWSE) {
            char pattern[64];
            std::snprintf(pattern, sizeof pattern, "*%s", archive_extension(m));
            ui_pick_file(m, "Install Mod Package", {pattern},
                         (mods && mods->archive_description)
                             ? mods->archive_description : "Mod package",
                         [m, mods](const char* path) {
                if (!path) return;
                if (!mods || !mods->install_archive ||
                    !mods->install_archive(mods->ctx, path)) {
                    mod_note_error(m);
                    note(m->mod_status);
                } else {
                    note("INSTALLED. SWITCH IT ON IN THE LIST.");
                }
            });
        } else if (cur - nf - nn == ADD_FOLDER) {
            if (!moa_menu_open_folder(new_mods_dir()))
                note("COULD NOT OPEN THE FOLDER");
        } else {
            g_state.classic_mods = true;
        }
    } else if (in.alt && cur < nf) {
        remove_feature_package(m, features[cur]);
    }
    if (in.back) launcher_model_set_view(m, LNG_VIEW_DASHBOARD);
}

#include "moa_menu_net.inc"

/* ---- entry points -------------------------------------------------------------- */

/* True when this menu, not the stock page, draws the current view. */
static bool owns_view(const LauncherModel* m) {
    if (m->view != LNG_VIEW_DASHBOARD) g_state.ai_page = g_state.extras_page = g_state.updates_page = false;
    if (m->view == LNG_VIEW_DASHBOARD) {
        g_state.classic_settings = false;
        g_state.classic_mods = false;
        g_state.classic_net = false;
        g_state.direct = false;
        return true;
    }
    if (m->view == LNG_VIEW_NETPLAY_MODE) {
        /* The stock pages' Back lands here, which ends the detour. */
        g_state.classic_net = false;
        g_state.direct = false;
        return m->netplay_supported;
    }
    if (m->view == LNG_VIEW_NETPLAY || m->view == LNG_VIEW_LOBBY)
        return m->netplay_supported && !g_state.classic_net;
    if (m->view == LNG_VIEW_SETTINGS) return !g_state.classic_settings;
    if (m->view == LNG_VIEW_MODS)     return !g_state.classic_mods && m->mods != nullptr;
    return false;
}

static void draw(LauncherModel* m, const LauncherTheme& th) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    Gfx g;
    g.dl = ImGui::GetWindowDrawList();
    g.font = ImGui::GetFont();
    g.s = std::min(vp->Size.x / kW, vp->Size.y / kH);
    if (g.s <= 0.0f) g.s = 1.0f;
    g.o = ImVec2(vp->Pos.x + (vp->Size.x - kW * g.s) * 0.5f,
                 vp->Pos.y + (vp->Size.y - kH * g.s) * 0.5f);

    /* Ground and grid across the whole window, not just the design area. */
    const ImVec2 a = vp->Pos, b = ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
    g.dl->PushClipRect(a, b, false);
    g.dl->AddRectFilled(a, b, kBg);
    const float step = 32.0f * g.s;
    for (float x = std::fmod(g.o.x, step); x < b.x; x += step)
        g.dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), kGrid);
    for (float y = std::fmod(g.o.y, step); y < b.y; y += step)
        g.dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), kGrid);

    const Input in = read_input(m);
    if ((int)m->view != g_state.last_view) {   /* a note belongs to its page */
        g_state.last_view = (int)m->view;
        g_state.note[0] = '\0';
    }
    switch (m->view) {
        case LNG_VIEW_SETTINGS: draw_settings_page(m, g, in); break;
        case LNG_VIEW_MODS:     draw_mods_page(m, g, in); break;
        case LNG_VIEW_NETPLAY_MODE: draw_net_mode(m, g, in); break;
        case LNG_VIEW_NETPLAY:      draw_net_rooms(m, g, in); break;
        case LNG_VIEW_LOBBY:        draw_lobby_page(m, th, g, in); break;
        default:
            if (g_state.ai_page && !m->in_session) draw_ai_page(m, g, in);
            else if (g_state.extras_page && !m->in_session) draw_extras_page(m, g, in);
            else if (g_state.updates_page && !m->in_session) draw_updates_page(g, in);
            else draw_main(m, g, in);
            break;
    }
    g.dl->PopClipRect();
}

} /* namespace moa_menu */
