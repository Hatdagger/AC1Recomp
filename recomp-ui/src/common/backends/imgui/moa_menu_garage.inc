/* EXTRA GARAGE: the launcher's own garage, around one shared AC (g_garage).
 *
 *   ASSEMBLY            the game's Assembly screen on the garage AC
 *   OPTIONAL PARTS      the game's Optional Parts screen, plus HUMAN PLUS
 *   DESIGN              PAINT, EMBLEM, AC NAME, PILOT NAME
 *   AI PROGRAMMING      the garage AC's AI (travels with it as an AI card)
 *   MEMORY CARD         memory card manager (below)
 *   AI CARD MANAGEMENT  the AI card list: edit, rename, delete, AI settings
 *   AI ARENA SETUP      PLAYER VS AI / AI VS AI
 *
 * MEMORY CARD works on the .mcd files in the memory card folder (L1 / R1 switch
 * between them). Only Master of Arena saves (BASLUS-0103x) can be loaded,
 * exported or deleted; other games' saves are listed and left alone. Nothing
 * is ever overwritten: new saves go into a free block, and every change keeps
 * the previous image as <card>.bak.
 *
 * Included by moa_menu.inc after the AI pages.
 */

static bool g_moved(bool hovered) {
    return hovered && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f);
}

enum GarageRow { G_ASSEMBLY = 0, G_OPTIONS, G_DESIGN, G_AI, G_DATA, G_CARDS, G_ARENA, G__COUNT };
enum DesignRow { D_PAINT = 0, D_EMBLEM, D_AC_NAME, D_PILOT, D__COUNT };
static int g_garage_sub = 0;   /* 0 the list, 1 MEMORY CARD, 2 AI ARENA SETUP, 3 DESIGN */

static void open_garage_page(LauncherModel* m) {
    (void)m;
    load_garage();
    g_state.garage_page = true;
    g_garage_sub = 0;
    g_state.note[0] = '\0';
}

/* ---- DATA: the memory card manager ------------------------------------------ */

struct McSave {
    int slot = 0;            /* first directory frame, 1..15 */
    int blocks = 1;
    bool moa = false;        /* a Master of Arena save */
    std::string code;        /* product code, e.g. BASLUS-01030A */
    bool has_card = false;   /* the AC read from it */
    moa_card card;
};

static std::vector<std::string> g_dc_files;   /* .mcd files in the memory card folder */
static int g_dc_file = 0;
static std::vector<uint8_t> g_dc_image;       /* the chosen card, 128 KB */
static bool g_dc_ok = false;
static std::vector<McSave> g_dc_saves;
static std::vector<bool> g_dc_marked;
static bool g_dc_details = false;
static int  g_dc_menu = -1;                   /* action menu open: its cursor */
static bool g_dc_confirm = false;             /* delete: asking */
static int  g_dc_import = 0;                  /* IMPORT AI CARD: which card */
static std::vector<moa_story_save> g_dc_story; /* story saves on the chosen card */
static int  g_dc_story_pick = 0;              /* PUT INTO STORY SAVE: which save */
static bool g_dc_story_confirm = false;       /* PUT INTO STORY SAVE: asking */

static uint32_t mc_u32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void dc_read_card() {
    g_dc_saves.clear();
    g_dc_story.clear();
    g_dc_ok = false;
    g_dc_image.assign(131072, 0);
    if (g_dc_files.empty()) return;
    std::FILE* f = std::fopen(g_dc_files[(size_t)g_dc_file].c_str(), "rb");
    if (!f) return;
    const size_t got = std::fread(g_dc_image.data(), 1, g_dc_image.size(), f);
    std::fclose(f);
    if (got != 131072 || g_dc_image[0] != 'M' || g_dc_image[1] != 'C') return;
    g_dc_ok = true;
    for (int i = 1; i < 16; ++i) {
        const uint8_t* fr = g_dc_image.data() + i * 128;
        if ((mc_u32(fr) & 0xFF) != 0x51) continue;           /* first block of a save */
        McSave s;
        s.slot = i;
        const uint32_t size = mc_u32(fr + 4);
        s.blocks = size >= 8192 ? (int)(size / 8192) : 1;
        char code[21] = {0};
        std::memcpy(code, fr + 10, 20);
        for (char& c : code) if (c && (c < 32 || c > 126)) c = '?';
        s.code = code;
        s.moa = std::strncmp(code, "BASLUS-0103", 11) == 0;
        if (s.moa)
            s.has_card = moa_card_from_save(g_dc_image.data() + i * 8192, 8192, &s.card) != 0;
        g_dc_saves.push_back(s);
    }
    g_dc_marked.assign(g_dc_saves.size(), false);
    moa_story_save list[15];
    const int n = moa_memcard_list_story(g_dc_files[(size_t)g_dc_file].c_str(), list, 15);
    for (int i = 0; i < n; ++i) g_dc_story.push_back(list[i]);
    if (g_dc_story_pick >= (int)g_dc_story.size()) g_dc_story_pick = 0;
}

static void dc_scan(LauncherModel* m) {
    namespace fs = std::filesystem;
    const std::string keep = g_dc_files.empty() ? std::string() : g_dc_files[(size_t)g_dc_file];
    g_dc_files.clear();
    const std::string folder = ai_card_folder(m);
    std::error_code ec;
    if (!folder.empty())
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code fec;
            if (!it->is_regular_file(fec)) continue;
            if (upper(it->path().extension().u8string().c_str()) == ".MCD")
                g_dc_files.push_back(it->path().u8string());
        }
    std::sort(g_dc_files.begin(), g_dc_files.end());
    const char* card1 = m->s.memcard_path[0];
    if (card1 && card1[0] && std::find(g_dc_files.begin(), g_dc_files.end(), card1) == g_dc_files.end())
        g_dc_files.insert(g_dc_files.begin(), card1);   /* card 1 even before its first save */
    g_dc_file = 0;
    const std::string want = keep.empty() && card1 ? std::string(card1) : keep;
    for (size_t i = 0; i < g_dc_files.size(); ++i)
        if (g_dc_files[i] == want) g_dc_file = (int)i;
    dc_read_card();
}

static void open_data_page(LauncherModel* m) {
    scan_ai_cards(m);
    g_garage_sub = 1;
    g_dc_details = false;
    g_dc_menu = -1;
    g_dc_confirm = false;
    g_state.cursor[PAGE_AI] = 0;
    dc_scan(m);
}

static std::string dc_path() {
    return g_dc_files.empty() ? std::string() : g_dc_files[(size_t)g_dc_file];
}

static bool dc_write_with_backup() {
    const std::string path = dc_path();
    /* Keep the image as it is on disk now as the backup. */
    std::vector<uint8_t> old(131072);
    std::FILE* f = std::fopen(path.c_str(), "rb");
    size_t got = 0;
    if (f) { got = std::fread(old.data(), 1, old.size(), f); std::fclose(f); }
    if (got == old.size()) {
        std::FILE* b = std::fopen((path + ".bak").c_str(), "wb");
        const bool ok = b && std::fwrite(old.data(), 1, old.size(), b) == old.size();
        if (b && std::fclose(b) != 0) return false;
        if (!ok) return false;
    }
    f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(g_dc_image.data(), 1, g_dc_image.size(), f) == g_dc_image.size();
    return std::fclose(f) == 0 && ok;
}

/* Free a save's blocks (marked deleted, as the console does). */
static void dc_free_save(int first) {
    int i = first;
    for (int guard = 0; i >= 1 && i < 16 && guard < 15; ++guard) {
        uint8_t* fr = g_dc_image.data() + i * 128;
        fr[0] = (uint8_t)((fr[0] & 0x0F) | 0xA0);
        uint8_t x = 0;
        for (int k = 0; k < 127; ++k) x ^= fr[k];
        fr[127] = x;
        const int next = fr[8] | fr[9] << 8;
        if (next == 0xFFFF) break;
        i = next + 1;
    }
}

/* The saves an action applies to: the marked ones, else the one selected. */
static std::vector<int> dc_targets(int cur_save) {
    std::vector<int> v;
    for (size_t i = 0; i < g_dc_marked.size(); ++i) if (g_dc_marked[i]) v.push_back((int)i);
    if (v.empty() && cur_save >= 0) v.push_back(cur_save);
    return v;
}

enum DcRow { DC_SAVE_GARAGE = 0, DC_STORY, DC_IMPORT, DC_TO_CARD, DC_SAVES };
enum DcAction { DA_LOAD = 0, DA_EXPORT, DA_DELETE, DA_CANCEL, DA__COUNT };

static void dc_action(LauncherModel* m, int action, int cur_save) {
    const std::vector<int> t = dc_targets(cur_save);
    char buf[120];
    if (action == DA_LOAD) {
        if (t.size() != 1) { note("LOAD ONE SAVE AT A TIME"); return; }
        const McSave& s = g_dc_saves[(size_t)t[0]];
        if (!s.has_card) { note("ONLY MASTER OF ARENA SAVES CAN BE LOADED"); return; }
        g_garage = s.card;
        if (!g_garage.has_ai) moa_card_set_preset(&g_garage, MOA_AI_PRESET_BALANCED);
        note(save_garage() ? "LOADED INTO THE GARAGE" : "THE GARAGE AC COULD NOT BE WRITTEN");
    } else if (action == DA_EXPORT) {
        const std::string folder = ai_card_folder(m);
        int n = 0;
        for (int k : t) {
            const McSave& s = g_dc_saves[(size_t)k];
            if (!s.has_card) continue;
            const std::string path = unused_card_path(folder, s.card.ac);
            if (!path.empty() && moa_card_write_png(&s.card, path.c_str())) ++n;
        }
        scan_ai_cards(m);
        std::snprintf(buf, sizeof buf, "EXPORTED %d AI CARD%s", n, n == 1 ? "" : "S");
        note(n ? buf : "NOTHING EXPORTED (ONLY MASTER OF ARENA SAVES)");
    } else if (action == DA_DELETE) {
        int n = 0;
        for (int k : t) if (g_dc_saves[(size_t)k].moa) { dc_free_save(g_dc_saves[(size_t)k].slot); ++n; }
        if (!n) { note("ONLY MASTER OF ARENA SAVES CAN BE DELETED HERE"); return; }
        if (!dc_write_with_backup()) { note("THE MEMORY CARD COULD NOT BE WRITTEN"); dc_read_card(); return; }
        std::snprintf(buf, sizeof buf, "DELETED %d SAVE%s (BACKUP KEPT)", n, n == 1 ? "" : "S");
        note(buf);
        dc_read_card();
    }
}

static void draw_save_info(const Gfx& g, const McSave& s, bool details) {
    if (!s.has_card) {
        text(g, 324.0f, 92.0f, 12.0f, kBright, s.code.c_str());
        text(g, 324.0f, 112.0f, 10.0f, kDim, s.moa ? "MASTER OF ARENA SAVE (NO AC READ)" : "ANOTHER GAME'S SAVE");
        text(g, 324.0f, 128.0f, 10.0f, kText, "LEFT AS IT IS.");
        return;
    }
    const moa_card& c = s.card;
    if (!details) {
        draw_emblem(g, 324.0f, 92.0f, 96.0f, c);
        text(g, 432.0f, 92.0f, 13.0f, kBright, upper(c.ac).c_str());
        text(g, 432.0f, 112.0f, 10.0f, kDim, "PILOT");
        text(g, 432.0f, 126.0f, 11.0f, kText, upper(c.pilot).c_str());
        text(g, 432.0f, 146.0f, 10.0f, kDim, s.code.c_str());
        char b[24];
        std::snprintf(b, sizeof b, "%d BLOCK%s", s.blocks, s.blocks == 1 ? "" : "S");
        text(g, 432.0f, 160.0f, 10.0f, kDim, b);
        return;
    }
    float y = 92.0f;
    text(g, 324.0f, y, 12.0f, kBright, upper(c.ac).c_str());
    y += 20.0f;
    for (int k = 0; k < kSlotCount; ++k) {
        const uint8_t id = c.rec[kSlots[k].byte];
        const moa_part* p = id == 255 ? nullptr
                          : moa_parts_find(&g_parts, moa_record_cat(kSlots[k].byte), id);
        text(g, 324.0f, y, 10.0f, kDim, kSlots[k].label);
        text(g, 424.0f, y, 10.0f, kText, id == 255 ? "NONE" : p ? p->name : "?");
        y += 14.0f;
    }
}

static void draw_data_page(LauncherModel* m, const Gfx& g, const Input& in) {
    parts_ready(m);
    const int rows = DC_SAVES + (int)g_dc_saves.size();
    const int visible = 12;
    const bool modal = g_dc_menu >= 0 || g_dc_confirm || g_dc_story_confirm;
    if (!modal) move_cursor(PAGE_AI, rows, in, visible);
    int& cur = g_state.cursor[PAGE_AI];
    if (cur >= rows) cur = rows - 1;
    int& top = g_state.top[PAGE_AI];
    const int cur_save = cur >= DC_SAVES ? cur - DC_SAVES : -1;

    char sub[160];
    std::snprintf(sub, sizeof sub, "%s  (%d / %d)",
                  g_dc_files.empty() ? "NO MEMORY CARD"
                      : upper(std::filesystem::path(dc_path()).filename().u8string().c_str()).c_str(),
                  g_dc_files.empty() ? 0 : g_dc_file + 1, (int)g_dc_files.size());
    header(g, "MEMORY CARD", sub);
    panel(g, 24.0f, 76.0f, 270.0f, 340.0f, "SAVES");
    int activate = -1;
    for (int i = top; i < rows && i < top + visible; ++i) {
        std::string label, value;
        bool enabled = g_dc_ok || g_dc_files.empty() || i == DC_SAVE_GARAGE || i == DC_TO_CARD;
        bool marked = false;
        if (i == DC_SAVE_GARAGE) { label = "SAVE AS VERSUS UNIT"; value = upper(g_garage.ac); }
        else if (i == DC_STORY) {
            label = "PUT INTO STORY SAVE";
            if (g_dc_story.empty()) { value = "NO STORY SAVE"; enabled = false; }
            else {
                const moa_story_save& t = g_dc_story[(size_t)(g_dc_story_pick % (int)g_dc_story.size())];
                value = upper(t.pilot[0] ? t.pilot : t.file);
            }
        }
        else if (i == DC_TO_CARD) { label = "EXPORT AS AI CARD"; value = upper(g_garage.ac); }
        else if (i == DC_IMPORT) {
            label = "IMPORT AI CARD";
            if (g_ai_cards.empty()) { value = "NO CARDS"; enabled = false; }
            else value = g_ai_cards[(size_t)(g_dc_import % (int)g_ai_cards.size())].label;
        } else {
            const McSave& s = g_dc_saves[(size_t)(i - DC_SAVES)];
            marked = g_dc_marked[(size_t)(i - DC_SAVES)];
            label = (marked ? "* " : "") + (s.has_card ? upper(s.card.ac) : s.code);
            value = s.moa ? "MOA" : "OTHER";
            enabled = s.moa;
        }
        bool hovered = false;
        if (row(g, 25.0f, 90.0f + kRowH * (i - top), 268.0f, i == cur, enabled, label.c_str(),
                value.c_str(), false, false, &hovered) && !modal)
            activate = i;
        if (!modal && g_moved(hovered)) cur = i;
    }

    panel(g, 310.0f, 76.0f, 306.0f, 216.0f, g_dc_details ? "PARTS" : "SAVE");
    if (cur_save >= 0) draw_save_info(g, g_dc_saves[(size_t)cur_save], g_dc_details);
    else if (cur == DC_SAVE_GARAGE)
        text(g, 324.0f, 92.0f, 10.0f, kText,
             "Writes the garage AC as a new versus unit in a free block: the AC "
             "only, no parts owned, so it is for VS mode (AC SETUP > SYSTEM > "
             "DATA LOAD), not for story mode. Nothing is overwritten.", 278.0f);
    else if (cur == DC_STORY) {
        if (g_dc_story.empty()) {
            text(g, 324.0f, 92.0f, 10.0f, kText,
                 "No story save on this card. Save in story mode first, then fit the "
                 "garage AC into it here.", 278.0f);
        } else {
            const moa_story_save& t = g_dc_story[(size_t)(g_dc_story_pick % (int)g_dc_story.size())];
            char b[96];
            text(g, 324.0f, 92.0f, 12.0f, kBright, upper(t.pilot[0] ? t.pilot : "PILOT").c_str());
            std::snprintf(b, sizeof b, "%s   %u C   %d PARTS OWNED", t.file, (unsigned)t.money, t.owned);
            text(g, 324.0f, 112.0f, 10.0f, kDim, b);
            text(g, 324.0f, 132.0f, 10.0f, kText,
                 "Fits the garage AC into this story save: parts, optional parts, "
                 "paint, emblem and AC name. Parts the save does not own are added "
                 "to it. Money, pilot, progress and Human PLUS stay. Left and right "
                 "pick the save; a backup of the card is kept.", 278.0f);
        }
    }
    else if (cur == DC_TO_CARD)
        text(g, 324.0f, 92.0f, 10.0f, kText,
             "Writes the garage AC, with its AI from AI PROGRAMMING, as a new AI "
             "card in the memory card folder. It then shows in AI CARD MANAGEMENT "
             "and AI ARENA SETUP.", 278.0f);
    else
        text(g, 324.0f, 92.0f, 10.0f, kText,
             "Writes the chosen AI card's AC as a new save in a free block. Left "
             "and right pick the card.", 278.0f);

    panel(g, 310.0f, 300.0f, 306.0f, 116.0f, "CARD");
    {
        char b[96];
        int used = 0;
        for (const McSave& s : g_dc_saves) used += s.blocks;
        if (g_dc_files.empty()) std::snprintf(b, sizeof b, "NO MEMORY CARD YET");
        else if (!g_dc_ok) std::snprintf(b, sizeof b, "NOT FORMATTED OR UNREADABLE");
        else std::snprintf(b, sizeof b, "%d / 15 BLOCKS USED", used);
        text(g, 324.0f, 316.0f, 11.0f, g_dc_ok ? kHi : kWarn, b);
        text(g, 324.0f, 336.0f, 10.0f, kDim,
             "Only Master of Arena saves can be loaded, exported or deleted. Every "
             "change keeps the previous card as a .bak file.", 278.0f);
    }

    if (g_dc_confirm) {
        panel(g, 160.0f, 170.0f, 320.0f, 90.0f, "DELETE");
        const size_t n = dc_targets(cur_save).size();
        char b[96];
        std::snprintf(b, sizeof b, "DELETE %d SAVE%s FROM THIS CARD?", (int)n, n == 1 ? "" : "S");
        text(g, 176.0f, 192.0f, 12.0f, kWarn, b);
        text(g, 176.0f, 216.0f, 10.0f, kText, "A  DELETE (A BACKUP IS KEPT)      B  CANCEL");
        footer(g, "A  DELETE      B  CANCEL");
        if (in.back) g_dc_confirm = false;
        else if (in.confirm) { g_dc_confirm = false; dc_action(m, DA_DELETE, cur_save); }
        return;
    }
    if (g_dc_story_confirm) {
        panel(g, 140.0f, 170.0f, 360.0f, 90.0f, "STORY SAVE");
        const moa_story_save& t = g_dc_story[(size_t)(g_dc_story_pick % (int)g_dc_story.size())];
        char b[120];
        std::snprintf(b, sizeof b, "REPLACE %s'S AC WITH %s?", upper(t.pilot[0] ? t.pilot : t.file).c_str(),
                      upper(g_garage.ac[0] ? g_garage.ac : "AC").c_str());
        text(g, 156.0f, 192.0f, 12.0f, kWarn, b, 330.0f);
        text(g, 156.0f, 216.0f, 10.0f, kText, "A  PUT IT IN (A BACKUP IS KEPT)      B  CANCEL");
        footer(g, "A  CONFIRM      B  CANCEL");
        if (in.back) { g_dc_story_confirm = false; return; }
        if (!in.confirm) return;
        g_dc_story_confirm = false;
        char err[160] = {0};
        int added = 0;
        if (moa_memcard_fit_story(dc_path().c_str(), t.slot, &g_garage, &added, err, sizeof err)) {
            std::snprintf(b, sizeof b, "PUT INTO THE STORY SAVE (%d PART%s ADDED). LOAD IT IN STORY MODE.",
                          added, added == 1 ? "" : "S");
            note(b);
        } else {
            note(upper(err[0] ? err : "THE MEMORY CARD COULD NOT BE WRITTEN").c_str());
        }
        dc_read_card();
        return;
    }
    if (g_dc_menu >= 0) {
        static const char* const kActs[DA__COUNT] = { "LOAD INTO GARAGE", "EXPORT AS AI CARD",
                                                       "DELETE", "CANCEL" };
        panel(g, 160.0f, 150.0f, 240.0f, 22.0f + kRowH * DA__COUNT, "SAVE");
        if (in.up) g_dc_menu = (g_dc_menu + DA__COUNT - 1) % DA__COUNT;
        if (in.down) g_dc_menu = (g_dc_menu + 1) % DA__COUNT;
        int pick = -1;
        for (int i = 0; i < DA__COUNT; ++i) {
            bool hovered = false;
            if (row(g, 161.0f, 164.0f + kRowH * i, 238.0f, i == g_dc_menu, true, kActs[i], "",
                    false, false, &hovered))
                pick = i;
            if (g_moved(hovered)) g_dc_menu = i;
        }
        footer(g, "A  SELECT      B  CANCEL");
        if (in.confirm) pick = g_dc_menu;
        if (in.back) pick = DA_CANCEL;
        if (pick < 0) return;
        g_dc_menu = -1;
        if (pick == DA_DELETE) g_dc_confirm = true;
        else if (pick != DA_CANCEL) dc_action(m, pick, cur_save);
        return;
    }

    footer(g, "A  SELECT      Y / TRIANGLE  DETAILS      X / SQUARE  MARK      "
              "L1 / R1  CARD      B  BACK");
    if (in.back) { g_garage_sub = 0; return; }
    if (in.info) g_dc_details = !g_dc_details;
    if ((in.page_up || in.page_down) && g_dc_files.size() > 1) {
        const int n = (int)g_dc_files.size();
        g_dc_file = (g_dc_file + (in.page_up ? 1 : n - 1)) % n;
        dc_read_card();
        cur = 0; top = 0;
        return;
    }
    if (cur == DC_STORY && !g_dc_story.empty() && (in.left || in.right)) {
        const int n = (int)g_dc_story.size();
        g_dc_story_pick = (g_dc_story_pick + (in.left ? n - 1 : 1)) % n;
    }
    if (cur == DC_IMPORT && !g_ai_cards.empty() && (in.left || in.right)) {
        const int n = (int)g_ai_cards.size();
        g_dc_import = (g_dc_import + (in.left ? n - 1 : 1)) % n;
    }
    if (in.alt && cur_save >= 0 && g_dc_saves[(size_t)cur_save].moa)
        g_dc_marked[(size_t)cur_save] = !g_dc_marked[(size_t)cur_save];
    if (in.confirm) activate = cur;
    if (activate < 0) return;
    cur = activate;
    char err[160] = {0};
    if (activate == DC_TO_CARD) {
        const std::string path = unused_card_path(ai_card_folder(m), g_garage.ac);
        if (!path.empty() && moa_card_write_png(&g_garage, path.c_str())) {
            scan_ai_cards(m);
            note("EXPORTED AS AN AI CARD");
        } else {
            note("THE AI CARD COULD NOT BE WRITTEN");
        }
        return;
    }
    if (activate == DC_STORY) {
        if (m->in_session) { note("CLOSE THE GAME FIRST"); return; }
        if (g_dc_story.empty()) { note("NO STORY SAVE ON THIS CARD"); return; }
        g_dc_story_confirm = true;
        return;
    }
    if (activate == DC_SAVE_GARAGE || activate == DC_IMPORT) {
        if (m->in_session) { note("CLOSE THE GAME FIRST"); return; }
        const std::string path = dc_path();
        if (path.empty()) { note("NO MEMORY CARD IS SET"); return; }
        const moa_card* c = &g_garage;
        if (activate == DC_IMPORT) {
            if (g_ai_cards.empty()) return;
            c = &g_ai_cards[(size_t)(g_dc_import % (int)g_ai_cards.size())].card;
        }
        if (moa_memcard_add_unit(path.c_str(), c, err, sizeof err)) {
            note(activate == DC_SAVE_GARAGE ? "SAVED AS A VERSUS UNIT ON THE CARD"
                                             : "SAVED AS A NEW SAVE ON THE CARD");
            dc_scan(m);
        } else {
            note(upper(err[0] ? err : "THE MEMORY CARD COULD NOT BE WRITTEN").c_str());
        }
        return;
    }
    if (cur_save >= 0 && g_dc_saves[(size_t)cur_save].moa) {
        if (m->in_session) { note("CLOSE THE GAME FIRST"); return; }
        g_dc_menu = 0;
    }
}

/* ---- the garage list -------------------------------------------------------- */

/* DESIGN > AC NAME / PILOT NAME: typed in place on the garage AC. */
static void garage_name_done() {
    g_garage = g_edit;
    if (!save_garage()) note("THE GARAGE AC COULD NOT BE WRITTEN");
}

static void draw_garage_page(LauncherModel* m, const Gfx& g, const Input& in) {
    if (g_garage_sub == 1) { draw_data_page(m, g, in); return; }
    const bool arena = g_garage_sub == 2, design = g_garage_sub == 3;
    const int count = arena ? 2 : design ? D__COUNT : G__COUNT;
    static int s_cur[3] = {0, 0, 0};
    int& cur = s_cur[arena ? 1 : design ? 2 : 0];
    const bool naming = design && g_name_edit != 0;
    if (!naming) {
        if (in.up) cur = (cur + count - 1) % count;
        if (in.down) cur = (cur + 1) % count;
    }

    static const char* const kRows[G__COUNT] = { "ASSEMBLY", "OPTIONAL PARTS", "DESIGN", "AI PROGRAMMING",
                                                 "MEMORY CARD", "AI CARD MANAGEMENT", "AI ARENA SETUP" };
    static const char* const kArena[2] = { "PLAYER VS AI", "AI VS AI" };
    static const char* const kDesign[D__COUNT] = { "PAINT", "EMBLEM", "AC NAME", "PILOT NAME" };
    static const char* const kInfo[G__COUNT] = {
        "The game's Assembly screen: change the garage AC's parts slot by slot. "
        "It is kept between launches.",
        "Fit optional parts into the core's slots, and switch HUMAN PLUS.",
        "Paint, emblem and the AC and pilot names.",
        "The garage AC's AI. It goes with the AC when you export it as an AI card "
        "(MEMORY CARD > EXPORT AS AI CARD).",
        "Memory card manager: load an AC from a save into the garage, save the "
        "garage AC as a versus unit or put it into a story save, export it as an "
        "AI card, export saves as AI cards, delete saves (a backup is kept).",
        "Your AI cards: edit their AI, rename, delete, and change their AC. "
        "Cards live in the memory card folder.",
        "Set up an arena match: PLAYER VS AI, where you fly unit 1, or AI VS AI.",
    };
    static const char* const kArenaInfo[2] = {
        "You fly unit 1 (the garage AC or an AI card's AC) against an AI card.",
        "Two AI cards fight each other while you watch.",
    };
    static const char* const kDesignInfo[D__COUNT] = {
        "The AC's colours: one scheme, or head, core, arms and legs apart.",
        "The emblem on the shoulder: a picture cut to the game's 64 x 64 in 16 colours.",
        "Up to 7 letters. Type, or up / down for the letter.",
        "Up to 7 letters. Type, or up / down for the letter.",
    };
    const char* const* labels = arena ? kArena : design ? kDesign : kRows;
    const char* const* info = arena ? kArenaInfo : design ? kDesignInfo : kInfo;

    header(g, arena ? "AI ARENA SETUP" : design ? "DESIGN" : "EXTRA GARAGE",
           arena || design ? "EXTRA GARAGE" : "LAUNCHER GARAGE");

    /* the game's GARAGE: tall boxed entries, the orange bar on the highlight */
    const float lx = 24.0f, top = 62.0f, lw = 250.0f, eh = 38.0f;
    int activate = -1;
    for (int i = 0; i < count; ++i) {
        const float y = top + (eh + 6.0f) * i;
        game_box(g, lx, y, lw, eh);
        if (i == cur) game_bar(g, lx + 4.0f, y + 4.0f, lw - 8.0f, eh - 8.0f, labels[i]);
        else {
            const float tw = text_width(g, 14.0f, labels[i]);
            text(g, lx + (lw - tw) * 0.5f, y + (eh - 14.0f) * 0.5f - 1.0f, 14.0f, kText, labels[i]);
        }
        if (design && (i == D_AC_NAME || i == D_PILOT)) {
            const int which = i == D_AC_NAME ? 1 : 2;
            const std::string v = name_shown(which);
            text_right(g, lx + lw - 10.0f, y + eh - 13.0f, 9.0f, g_name_edit == which ? kHi : kDim, upper(v).c_str());
        }
        const ImVec2 r0 = at(g, lx, y), r1 = at(g, lx + lw, y + eh);
        if (!naming && ImGui::IsMouseHoveringRect(r0, r1)) {
            if (g_moved(true)) cur = i;
            if (ImGui::IsMouseClicked(0)) activate = i;
        }
    }

    /* the preview window: the garage AC turning on black */
    const float rx = 290.0f, rw = 326.0f;
    game_box(g, rx, top, rw, 260.0f);
    draw_acv(g, rx + 2.0f, top + 2.0f, rw - 4.0f, 256.0f, design ? g_edit : g_garage);
    draw_emblem(g, rx + 10.0f, top + 10.0f, 32.0f, design ? g_edit : g_garage);
    text_right(g, rx + rw - 10.0f, top + 10.0f, 12.0f, kBright, upper(g_garage.ac).c_str());
    text_right(g, rx + rw - 10.0f, top + 26.0f, 10.0f, kDim, upper(g_garage.pilot).c_str());
    game_box(g, rx, top + 268.0f, rw, 86.0f);
    text(g, rx + 12.0f, top + 278.0f, 11.0f, kHi, labels[cur]);
    text(g, rx + 12.0f, top + 296.0f, 10.0f, kText, info[cur], rw - 24.0f);

    if (naming) {
        footer(g, "UP / DOWN  LETTER   LEFT / RIGHT  MOVE   X  DELETE   A  DONE  (or type)");
        name_input(in);
        if (!g_name_edit) garage_name_done();
        return;
    }
    footer(g, "A / ENTER  SELECT      D-PAD  MOVE      B / ESC  BACK");

    if (in.back) {
        if (arena || design) g_garage_sub = 0; else g_state.garage_page = false;
        return;
    }
    if (in.confirm) activate = cur;
    if (activate < 0) return;
    cur = activate;
    if (arena) { open_arena(m, activate == 0 ? 0 : 1); return; }
    if (design) {
        switch (activate) {
            case D_PAINT:   open_garage_screen(m, AI_SUB_PAINT); break;
            case D_EMBLEM:  open_garage_screen(m, AI_SUB_EMBLEM); break;
            case D_AC_NAME: case D_PILOT:
                g_edit = g_garage;
                g_name_edit = activate == D_AC_NAME ? 1 : 2;
                g_name_pos = 0;
                break;
        }
        return;
    }
    switch (activate) {
        case G_ASSEMBLY: open_assembly(m); break;
        case G_OPTIONS:  open_garage_screen(m, AI_SUB_OPTS); break;
        case G_DESIGN:   load_garage(); g_edit = g_garage; g_garage_sub = 3; s_cur[2] = 0; break;
        case G_AI:       open_garage_screen(m, AI_SUB_EDIT); break;
        case G_DATA:     open_data_page(m); break;
        case G_CARDS:    open_card_management(m); break;
        case G_ARENA:    g_garage_sub = 2; break;
    }
}
