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

namespace moa_menu {

enum Page {
    PAGE_MAIN = 0, PAGE_SETTINGS, PAGE_MODS,
    PAGE_NET_MODE, PAGE_NET_ROOMS, PAGE_LOBBY, PAGE__COUNT
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
    int    direct_field = 3;
    int    last_view = -1;
    double next_scan = 0.0;
    std::vector<std::string> new_packages;   /* files waiting in mods/new */
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

/* ---- main page ------------------------------------------------------------- */

enum MainEntry { MAIN_START = 0, MAIN_NETWORK, MAIN_SETTINGS, MAIN_MODS, MAIN_EXIT };

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

static void draw_main(LauncherModel* m, const Gfx& g, const Input& in) {
    struct Entry { MainEntry id; const char* label; const char* info; };
    Entry entries[5];
    int count = 0;
    entries[count++] = { MAIN_START, m->in_session ? "RESUME GAME" : "START GAME",
        m->in_session ? "Return to the game in progress."
                      : "Start Armored Core: Master of Arena with the settings "
                        "and mods chosen here." };
    if (m->netplay_supported && !m->in_session)
        entries[count++] = { MAIN_NETWORK, "NETWORK PLAY",
            "Versus over LAN or online, two players. The host is player 1; "
            "the guest is player 2 and brings their own memory card." };
    entries[count++] = { MAIN_SETTINGS, "SETTINGS",
        "Display, sound and controls." };
    if (m->mods)
        entries[count++] = { MAIN_MODS, "MODS",
            "Switch installed mods on or off, and install new ones." };
    entries[count++] = { MAIN_EXIT, m->in_session ? "QUIT GAME" : "EXIT",
        m->in_session ? "Close the game." : "Close the launcher." };

    move_cursor(PAGE_MAIN, count, in, count);
    int& cur = g_state.cursor[PAGE_MAIN];

    header(g, "MASTER OF ARENA", "SYSTEM MENU");
    panel(g, 24.0f, 76.0f, 270.0f, 340.0f, "SELECT");
    int activate = -1;
    for (int i = 0; i < count; ++i) {
        bool hovered = false;
        if (row(g, 25.0f, 90.0f + kRowH * i, 268.0f, i == cur, true,
                entries[i].label, nullptr, false, false, &hovered))
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
    }

    panel(g, 310.0f, 242.0f, 306.0f, 174.0f, "INFO");
    text(g, 324.0f, 258.0f, 11.0f, kHi, entries[cur].label);
    text(g, 324.0f, 280.0f, 11.0f, kText, entries[cur].info, 278.0f);

    footer(g, "A / ENTER  SELECT      D-PAD  MOVE      START  START GAME");

    if (activate < 0) return;
    cur = activate;
    switch (entries[activate].id) {
        case MAIN_START:    start_game(m); break;
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
        default:                draw_main(m, g, in); break;
    }
    g.dl->PopClipRect();
}

} /* namespace moa_menu */
