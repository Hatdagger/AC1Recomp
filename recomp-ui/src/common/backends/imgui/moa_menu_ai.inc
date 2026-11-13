/* Master of Arena system menu: AI Battle card workshop.
 *
 * Included by moa_menu.inc after the AI Battle page's card list (g_ai_cards,
 * scan_ai_cards, draw_emblem). Three sub-pages under AI BATTLE:
 *
 *   CARD WORKSHOP  the cards in the memory card folder; edit a card's AI,
 *                  duplicate it or delete it.
 *   AI EDITOR      PRESET or CUSTOM. CUSTOM is the game's own AI editor
 *                  (RANKER MK AI): REBEL POINT and six sliders, turned into
 *                  the 36-byte AI record exactly as the game does
 *                  (moa_ai_build_record).
 *   ADVANCED       every byte of that record; a changed byte is kept as an
 *                  override on top of the sliders.
 *
 * Saving redraws the card image (format 4) from the decoded card.
 */

enum AiSub { AI_SUB_SETUP = 0, AI_SUB_CARDS, AI_SUB_EDIT, AI_SUB_ADV };
static int g_ai_sub = AI_SUB_SETUP;

static moa_card g_edit;                 /* the card being edited */
static std::string g_edit_path;
static bool g_edit_dirty = false;
static int  g_ws_action = -1;           /* workshop: action list open on this card */
static int  g_ws_action_cur = 0;
static bool g_confirm_delete = false;
static bool g_confirm_discard = false;

static const char* const kSliderNames[MOA_AI_SLIDERS] = {
    "SHOOT FREQ.", "BLADE FREQ.", "LOCK FREQ.", "AVOID FREQ.", "FLY FREQ.", "RANGE TYPE"
};

/* As the game's editor shows the range: S / MID / L, distance from the middle. */
static std::string range_text(int v) {
    char b[16];
    if (v == 50) return "MID";
    std::snprintf(b, sizeof b, "%s %d%%", v < 50 ? "S" : "L", (v < 50 ? 50 - v : v - 50) * 2);
    return b;
}

static const char* rebel_text(int step) {
    static const char* const t[5] = { "0", "1", "2", "3", "FULL" };
    return t[step < 0 ? 0 : step > 4 ? 4 : step];
}

static std::string card_ai_label(const moa_card& c) {
    if (!c.has_ai) return "BALANCED*";
    std::string s = moa_ai_preset_name(c.ai_preset);
    for (int i = 1; i < MOA_AI_RECORD_SIZE; ++i)
        if (c.ai_set[i]) { s += " +ADV"; break; }
    return s;
}

/* What each record byte is known to do (arena engine 0x80055768, AI setup). */
static const char* adv_label(int i) {
    switch (i) {
        case 0:  return "HUMAN PLUS LEVEL";
        case 1:  return "GROUND CHANCE A";
        case 2:  return "GROUND CHANCE B";
        case 3:  return "FLY CHANCE";
        case 4:  return "REACTION RATE";
        case 5:  return "RANDOM ADD TO 1";
        case 6:  return "RANDOM ADD TO 2";
        case 7:  return "RANDOM ADD TO 3";
        case 8:  return "SETTING 604";
        case 9:  return "BLADE CHANCE";
        case 10: return "BLADE OFFSET";
        case 11: return "LOCK CHANCE";
        case 12: return "LOCK DELAY";
        case 13: return "SHOOT CHANCE";
        case 14: return "AVOID CHANCE";
        case 15: return "SETTING 620";
        case 16: return "DISTANCE A LOW";
        case 17: return "DISTANCE A HIGH";
        case 18: return "DISTANCE B LOW";
        case 19: return "DISTANCE B HIGH";
        default: break;
    }
    static char buf[24];
    if (i < 30) std::snprintf(buf, sizeof buf, "TABLE A %d / %c", (i - 20) / 2 + 1, (i & 1) ? 'B' : 'A');
    else        std::snprintf(buf, sizeof buf, "TABLE B %d / %c", (i - 30) / 2 + 1, (i & 1) ? 'B' : 'A');
    return buf;
}

static std::string unused_card_path(const std::string& folder, const char* ac) {
    std::string base;
    for (const char* p = ac; p && *p; ++p) {
        const char c = *p;
        base += (std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
    }
    if (base.empty()) base = "card";
    std::error_code ec;
    for (int n = 1; n < 1000; ++n) {
        char suffix[16];
        std::snprintf(suffix, sizeof suffix, n == 1 ? "" : "_%d", n);
        const std::filesystem::path p = std::filesystem::path(folder) / (base + suffix + ".png");
        if (!std::filesystem::exists(p, ec)) return p.string();
    }
    return std::string();
}

/* Rescan, keeping the cursor on `path` when it is still there. */
static void rescan_on(const LauncherModel* m, const std::string& path) {
    scan_ai_cards(m);
    for (size_t i = 0; i < g_ai_cards.size(); ++i)
        if (g_ai_cards[i].path == path) g_state.cursor[PAGE_AI_CARDS] = (int)i;
    if (g_state.cursor[PAGE_AI_CARDS] >= (int)g_ai_cards.size())
        g_state.cursor[PAGE_AI_CARDS] = g_ai_cards.empty() ? 0 : (int)g_ai_cards.size() - 1;
}

static void open_editor(const AiCard& c) {
    g_edit = c.card;
    g_edit_path = c.path;
    if (!g_edit.has_ai) moa_card_set_preset(&g_edit, MOA_AI_PRESET_BALANCED);
    g_edit_dirty = !c.card.has_ai;      /* an old card gains AI when saved */
    g_confirm_discard = false;
    g_ai_sub = AI_SUB_EDIT;
    g_state.cursor[PAGE_AI_EDIT] = 0;
}

/* ---- small drawing helpers ---- */

static void bar(const Gfx& g, float x, float y, float w, int v, ImU32 col) {
    g.dl->AddRect(at(g, x, y), at(g, x + w, y + 6.0f), kLine);
    g.dl->AddRectFilled(at(g, x + 1.0f, y + 1.0f),
                        at(g, x + 1.0f + (w - 2.0f) * (float)v / 100.0f, y + 5.0f), col);
}

static void ai_summary(const Gfx& g, float x, float y, const moa_card& c) {
    uint8_t s[MOA_AI_SLIDERS];
    if (c.has_ai) std::memcpy(s, c.ai_slider, sizeof s);
    else moa_ai_preset_sliders(MOA_AI_PRESET_BALANCED, s);
    text(g, x, y, 10.0f, kDim, "REBEL POINT");
    text_right(g, x + 250.0f, y, 10.0f, kText, rebel_text(moa_ai_rebel_step(c.plus)));
    for (int i = 0; i < MOA_AI_SLIDERS; ++i) {
        const float yy = y + 16.0f * (i + 1);
        text(g, x, yy, 10.0f, kDim, kSliderNames[i]);
        bar(g, x + 96.0f, yy + 4.0f, 100.0f, s[i], kHi);
        char v[16];
        if (i == MOA_AI_RANGE) std::snprintf(v, sizeof v, "%s", range_text(s[i]).c_str());
        else std::snprintf(v, sizeof v, "%d%%", s[i]);
        text_right(g, x + 250.0f, yy, 10.0f, kText, v);
    }
}

/* The 36 record bytes as a hex grid, overrides in orange. */
static void record_grid(const Gfx& g, float x, float y, const moa_card& c) {
    uint8_t rec[MOA_AI_RECORD_SIZE];
    moa_card_ai_record(&c, rec);
    for (int i = 0; i < MOA_AI_RECORD_SIZE; ++i) {
        char b[4];
        std::snprintf(b, sizeof b, "%02X", rec[i]);
        text(g, x + (i % 12) * 21.0f, y + (i / 12) * 14.0f, 10.0f,
             (c.has_ai && c.ai_set[i]) ? kHi : kText, b);
    }
}

/* ---- CARD WORKSHOP ---- */

static void draw_ai_cards(LauncherModel* m, const Gfx& g, const Input& in) {
    const int n = (int)g_ai_cards.size();
    const int visible = 11;
    header(g, "CARD WORKSHOP", "AI BATTLE CARDS");
    panel(g, 24.0f, 76.0f, 300.0f, 314.0f, "CARDS");
    if (g_ws_action < 0 && !g_confirm_delete) move_cursor(PAGE_AI_CARDS, n, in, visible);
    int& cur = g_state.cursor[PAGE_AI_CARDS];
    const int top = g_state.top[PAGE_AI_CARDS];
    if (n == 0) {
        text(g, 38.0f, 96.0f, 11.0f, kWarn, "NO CARDS IN THE FOLDER", 270.0f);
        text(g, 38.0f, 116.0f, 10.0f, kText,
             "Make one with the Raven Card Builder or export your saves "
             "from the AI BATTLE page.", 270.0f);
    }
    for (int i = top; i < n && i < top + visible; ++i) {
        bool hovered = false;
        if (row(g, 25.0f, 90.0f + kRowH * (i - top), 298.0f, i == cur, true,
                g_ai_cards[(size_t)i].label.c_str(), card_ai_label(g_ai_cards[(size_t)i].card).c_str(),
                false, false, &hovered) && g_ws_action < 0 && !g_confirm_delete) {
            cur = i;
            g_ws_action = i;
            g_ws_action_cur = 0;
        }
        if (hovered && g_ws_action < 0 && (ImGui::GetIO().MouseDelta.x != 0.0f ||
                                           ImGui::GetIO().MouseDelta.y != 0.0f))
            cur = i;
    }

    panel(g, 340.0f, 76.0f, 276.0f, 314.0f, "CARD");
    if (cur >= 0 && cur < n) {
        const AiCard& c = g_ai_cards[(size_t)cur];
        draw_emblem(g, 354.0f, 92.0f, 72.0f, c.card);
        text(g, 436.0f, 94.0f, 13.0f, kBright, c.label.c_str());
        text(g, 436.0f, 114.0f, 10.0f, kDim, "PILOT");
        text(g, 436.0f, 128.0f, 11.0f, kText, upper(c.card.pilot).c_str());
        text(g, 436.0f, 146.0f, 10.0f, kDim, "AI");
        text(g, 456.0f, 146.0f, 10.0f, kHi, card_ai_label(c.card).c_str());
        ai_summary(g, 354.0f, 180.0f, c.card);
        if (!c.card.has_ai)
            text(g, 354.0f, 296.0f, 10.0f, kDim,
                 "* No AI on this card yet: it flies BALANCED. Edit and save to give it one.",
                 250.0f);
        text(g, 354.0f, 334.0f, 10.0f, kDim,
             std::filesystem::path(c.path).filename().string().c_str(), 250.0f);
    }

    /* Action list / delete prompt over the card panel. */
    static const char* const kActions[] = { "EDIT AI", "DUPLICATE", "DELETE", "CANCEL" };
    if (g_confirm_delete && cur >= 0 && cur < n) {
        panel(g, 360.0f, 220.0f, 236.0f, 80.0f, "DELETE CARD");
        text(g, 374.0f, 236.0f, 11.0f, kWarn, ("DELETE " + g_ai_cards[(size_t)cur].label + "?").c_str(), 210.0f);
        text(g, 374.0f, 270.0f, 10.0f, kText, "A  DELETE      B  KEEP");
    } else if (g_ws_action >= 0) {
        panel(g, 360.0f, 200.0f, 236.0f, 4.0f * kRowH + 20.0f, "CARD");
        for (int i = 0; i < 4; ++i)
            row(g, 361.0f, 210.0f + kRowH * i, 234.0f, i == g_ws_action_cur, true, kActions[i], "",
                false, false, nullptr);
    }
    footer(g, g_ws_action >= 0 || g_confirm_delete ? "A  SELECT      B  BACK"
                                                   : "A  CARD MENU      X  DELETE      B  BACK");

    if (g_confirm_delete) {
        if (in.back) { g_confirm_delete = false; return; }
        if (!in.confirm || cur < 0 || cur >= n) return;
        g_confirm_delete = false;
        const std::string path = g_ai_cards[(size_t)cur].path;
        std::error_code ec;
        if (std::filesystem::remove(path, ec)) {
            for (std::string& chosen : g_ai_chosen) if (chosen == path) chosen.clear();
            note("CARD DELETED");
        } else {
            note("COULD NOT DELETE THE CARD");
        }
        rescan_on(m, std::string());
        return;
    }
    if (g_ws_action >= 0) {
        if (in.up)   g_ws_action_cur = (g_ws_action_cur + 3) % 4;
        if (in.down) g_ws_action_cur = (g_ws_action_cur + 1) % 4;
        if (in.back) { g_ws_action = -1; return; }
        if (!in.confirm) return;
        const int idx = g_ws_action;
        g_ws_action = -1;
        if (idx < 0 || idx >= n) return;
        if (g_ws_action_cur == 0) { open_editor(g_ai_cards[(size_t)idx]); return; }
        if (g_ws_action_cur == 1) {
            const std::string path = unused_card_path(g_ai_folder, g_ai_cards[(size_t)idx].card.ac);
            if (!path.empty() && moa_card_write_png(&g_ai_cards[(size_t)idx].card, path.c_str())) {
                rescan_on(m, path);
                note("CARD DUPLICATED");
            } else {
                note("COULD NOT WRITE THE CARD");
            }
            return;
        }
        if (g_ws_action_cur == 2) g_confirm_delete = true;
        return;
    }
    if (in.back) { g_ai_sub = AI_SUB_SETUP; return; }
    if (n == 0) return;
    if (in.alt) { g_confirm_delete = true; return; }
    if (in.confirm) { g_ws_action = cur; g_ws_action_cur = 0; }
}

/* ---- AI EDITOR ---- */

static bool save_edit(LauncherModel* m, bool as_new) {
    const std::string path = as_new ? unused_card_path(g_ai_folder, g_edit.ac) : g_edit_path;
    if (path.empty() || !moa_card_write_png(&g_edit, path.c_str())) {
        note("COULD NOT WRITE THE CARD");
        return false;
    }
    g_edit_path = path;
    g_edit_dirty = false;
    rescan_on(m, path);
    note(as_new ? "SAVED AS A NEW CARD" : "CARD SAVED");
    return true;
}

static void draw_ai_edit(LauncherModel* m, const Gfx& g, const Input& in) {
    enum { E_MODE = 0, E_PRESET, E_REBEL, E_S0, E_ADV = E_S0 + MOA_AI_SLIDERS,
           E_SAVE, E_SAVE_NEW, E_REVERT, E_ROWS };
    if (!g_confirm_discard) move_cursor(PAGE_AI_EDIT, E_ROWS, in, E_ROWS);
    int& cur = g_state.cursor[PAGE_AI_EDIT];
    const bool custom = g_edit.ai_preset == MOA_AI_PRESET_CUSTOM;
    int overrides = 0;
    for (int i = 1; i < MOA_AI_RECORD_SIZE; ++i) overrides += g_edit.ai_set[i] ? 1 : 0;

    header(g, "AI EDITOR", upper(g_edit.ac).c_str());
    panel(g, 24.0f, 66.0f, 300.0f, 14.0f * kRowH + 14.0f, g_edit_dirty ? "AI  (NOT SAVED)" : "AI");
    int activate = -1;
    for (int i = 0; i < E_ROWS; ++i) {
        std::string label, value;
        bool enabled = true;
        if (i == E_MODE) { label = "MODE"; value = custom ? "CUSTOM" : "PRESET"; }
        else if (i == E_PRESET) {
            label = "PRESET";
            value = custom ? "-" : moa_ai_preset_name(g_edit.ai_preset);
            enabled = !custom;
        } else if (i == E_REBEL) {
            label = "REBEL POINT";
            value = rebel_text(moa_ai_rebel_step(g_edit.plus));
        } else if (i < E_ADV) {
            const int k = i - E_S0;
            label = kSliderNames[k];
            char v[16];
            if (k == MOA_AI_RANGE) std::snprintf(v, sizeof v, "%s", range_text(g_edit.ai_slider[k]).c_str());
            else std::snprintf(v, sizeof v, "%d%%", g_edit.ai_slider[k]);
            value = v;
        } else if (i == E_ADV) {
            label = "ADVANCED";
            char v[24];
            std::snprintf(v, sizeof v, overrides ? "%d CHANGED" : "-", overrides);
            value = v;
        } else if (i == E_SAVE) { label = "SAVE CARD"; }
        else if (i == E_SAVE_NEW) { label = "SAVE AS NEW CARD"; }
        else { label = "REVERT"; enabled = g_edit_dirty; }
        bool hovered = false;
        if (row(g, 25.0f, 78.0f + kRowH * i, 298.0f, i == cur, enabled, label.c_str(),
                value.c_str(), false, false, &hovered) && !g_confirm_discard)
            activate = i;
        if (hovered && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f))
            cur = i;
        if (i >= E_S0 && i < E_ADV)
            bar(g, 150.0f, 78.0f + kRowH * i + 11.0f, 90.0f, g_edit.ai_slider[i - E_S0],
                i == cur ? kHi : kDim);
    }

    panel(g, 340.0f, 66.0f, 276.0f, 14.0f * kRowH + 14.0f, "CARD");
    draw_emblem(g, 354.0f, 82.0f, 72.0f, g_edit);
    text(g, 436.0f, 84.0f, 13.0f, kBright, upper(g_edit.ac).c_str());
    text(g, 436.0f, 104.0f, 10.0f, kDim, "PILOT");
    text(g, 436.0f, 118.0f, 11.0f, kText, upper(g_edit.pilot).c_str());
    text(g, 354.0f, 172.0f, 10.0f, kDim, "AI RECORD (WHAT THE GAME READS)");
    record_grid(g, 354.0f, 190.0f, g_edit);
    text(g, 354.0f, 246.0f, 10.0f, kText,
         "The sliders are the game's own RANKER MK AI editor and change the record "
         "the same way it does. REBEL POINT is the Human PLUS level (FULL = 9). "
         "Moving a slider switches to CUSTOM.", 250.0f);
    if (g_edit.plus == 0 && !g_confirm_discard)
        text(g, 354.0f, 330.0f, 10.0f, kWarn,
             "REBEL POINT 0: the load and energy limits apply. A build over them "
             "will not start the match.", 250.0f);
    if (g_confirm_discard) {
        panel(g, 360.0f, 330.0f, 236.0f, 64.0f, "UNSAVED CHANGES");
        text(g, 374.0f, 346.0f, 11.0f, kWarn, "LEAVE WITHOUT SAVING?");
        text(g, 374.0f, 370.0f, 10.0f, kText, "A  LEAVE      B  STAY");
    }
    footer(g, "A  SELECT      LEFT / RIGHT  CHANGE (L1 / R1 FINE)      B  BACK");

    if (g_confirm_discard) {
        if (in.back) { g_confirm_discard = false; return; }
        if (in.confirm) { g_confirm_discard = false; g_edit_dirty = false; g_ai_sub = AI_SUB_CARDS; }
        return;
    }
    if (in.back) {
        if (g_edit_dirty) g_confirm_discard = true;
        else g_ai_sub = AI_SUB_CARDS;
        return;
    }
    int dir = in.left ? -1 : in.right ? 1 : 0;
    int fine = in.page_down ? -1 : in.page_up ? 1 : 0;
    if (in.confirm) activate = cur;
    if (dir || fine) {
        if (cur == E_MODE && dir) {
            if (custom) g_edit.ai_preset = MOA_AI_PRESET_BALANCED,
                        moa_ai_preset_sliders(g_edit.ai_preset, g_edit.ai_slider);
            else g_edit.ai_preset = MOA_AI_PRESET_CUSTOM;
            g_edit_dirty = true;
        } else if (cur == E_PRESET && !custom && dir) {
            int p = g_edit.ai_preset + dir;
            if (p < 1) p = MOA_AI_PRESETS - 1;
            if (p >= MOA_AI_PRESETS) p = 1;
            g_edit.ai_preset = (uint8_t)p;
            moa_ai_preset_sliders(p, g_edit.ai_slider);
            g_edit_dirty = true;
        } else if (cur == E_REBEL && dir) {
            int s = moa_ai_rebel_step(g_edit.plus) + dir;
            s = s < 0 ? 4 : s > 4 ? 0 : s;
            g_edit.plus = moa_ai_rebel_level(s);
            g_edit_dirty = true;
        } else if (cur >= E_S0 && cur < E_ADV) {
            uint8_t& v = g_edit.ai_slider[cur - E_S0];
            const int nv = std::clamp((int)v + dir * 5 + fine, 0, 100);
            if (nv != v) {
                v = (uint8_t)nv;
                g_edit.ai_preset = MOA_AI_PRESET_CUSTOM;
                g_edit_dirty = true;
            }
        }
        return;
    }
    if (activate < 0) return;
    cur = activate;
    switch (activate) {
        case E_MODE:
            if (custom) g_edit.ai_preset = MOA_AI_PRESET_BALANCED,
                        moa_ai_preset_sliders(g_edit.ai_preset, g_edit.ai_slider);
            else g_edit.ai_preset = MOA_AI_PRESET_CUSTOM;
            g_edit_dirty = true;
            break;
        case E_ADV:
            g_ai_sub = AI_SUB_ADV;
            g_state.cursor[PAGE_AI_ADV] = 1;
            g_state.top[PAGE_AI_ADV] = 0;
            break;
        case E_SAVE: save_edit(m, false); break;
        case E_SAVE_NEW: save_edit(m, true); break;
        case E_REVERT:
            for (const AiCard& c : g_ai_cards)
                if (c.path == g_edit_path) { open_editor(c); break; }
            break;
        default: break;
    }
}

/* ---- ADVANCED ---- */

static void draw_ai_adv(const Gfx& g, const Input& in) {
    const int visible = 12;
    move_cursor(PAGE_AI_ADV, MOA_AI_RECORD_SIZE, in, visible);
    int& cur = g_state.cursor[PAGE_AI_ADV];
    if (cur == 0) cur = 1;                      /* byte 0 is REBEL POINT on the editor */
    const int top = g_state.top[PAGE_AI_ADV];
    uint8_t rec[MOA_AI_RECORD_SIZE];
    moa_card_ai_record(&g_edit, rec);

    header(g, "ADVANCED", "AI RECORD BYTES");
    panel(g, 24.0f, 66.0f, 380.0f, visible * kRowH + 14.0f, g_edit_dirty ? "RECORD  (NOT SAVED)" : "RECORD");
    for (int i = top; i < MOA_AI_RECORD_SIZE && i < top + visible; ++i) {
        char label[40], value[16];
        std::snprintf(label, sizeof label, "%02d  %s", i, adv_label(i));
        std::snprintf(value, sizeof value, "%s%d", g_edit.ai_set[i] ? "* " : "", rec[i]);
        row(g, 25.0f, 78.0f + kRowH * (i - top), 378.0f, i == cur, i != 0, label, value,
            false, false, nullptr);
    }
    panel(g, 420.0f, 66.0f, 196.0f, visible * kRowH + 14.0f, "HELP");
    text(g, 434.0f, 84.0f, 10.0f, kText,
         "Raw bytes of the AI record the arena engine reads. A byte you change "
         "here (marked *) stays fixed on top of the sliders. Names come from "
         "how the game's AI setup uses each byte; bytes the game's own editor "
         "never touches are experimental.", 170.0f);
    text(g, 434.0f, 230.0f, 10.0f, kHi, "A  BACK TO SLIDER VALUE", 170.0f);
    footer(g, "LEFT / RIGHT  1      L1 / R1  10      A  RESET BYTE      B  BACK");

    if (in.back) { g_ai_sub = AI_SUB_EDIT; return; }
    const int step = in.left ? -1 : in.right ? 1 : in.page_down ? -10 : in.page_up ? 10 : 0;
    if (cur > 0 && step) {
        g_edit.has_ai = 1;
        g_edit.ai_set[cur] = 1;
        g_edit.ai_raw[cur] = (uint8_t)std::clamp((int)rec[cur] + step, 0, 255);
        g_edit_dirty = true;
    }
    if (cur > 0 && in.confirm && g_edit.ai_set[cur]) {
        g_edit.ai_set[cur] = 0;
        g_edit_dirty = true;
    }
}
