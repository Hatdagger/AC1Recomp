/* START GAME: three ways in (included by moa_menu.inc).
 *
 *   LOAD GAME    pick a Scenario save on memory card 1; the game opens on the
 *                hub with it loaded (no title, no card screens)
 *   NEW GAME     straight to the game's name entry
 *   LAUNCH GAME  the title screen; FULL BOOT off (default) skips the logos
 *                and intro movie, on plays the whole boot
 *
 * The game side is games/master-of-arena/moa_quick_start.c (PSX_MOA_START,
 * PSX_MOA_START_SAVE, PSX_MOA_FULL_BOOT). start_game() clears PSX_MOA_START
 * for every other way of starting, so Rogue Arena, AI Battle and netplay keep
 * their own boots.
 */

static std::string g_quick_start;               /* read by start_game() once */
static bool g_start_page = false;
static bool g_start_list = false;               /* the LOAD GAME save list is up */
static bool g_full_boot = false;
static bool g_full_boot_read = false;
static std::vector<moa_story_save> g_start_saves;
static bool g_start_saves_ok = false;

static std::string start_prefs_path() { return asset("saves/moa_start.txt"); }

static void full_boot_read() {
    if (g_full_boot_read) return;
    g_full_boot_read = true;
    std::FILE* f = std::fopen(start_prefs_path().c_str(), "rb");
    if (!f) return;
    char line[64];
    while (std::fgets(line, sizeof line, f))
        if (std::strncmp(line, "full_boot=", 10) == 0) g_full_boot = line[10] == '1';
    std::fclose(f);
}

static void full_boot_toggle() {
    g_full_boot = !g_full_boot;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(start_prefs_path()).parent_path(), ec);
    std::FILE* f = std::fopen(start_prefs_path().c_str(), "wb");
    if (f) { std::fprintf(f, "full_boot=%d\n", g_full_boot ? 1 : 0); std::fclose(f); }
}

/* Memory card 1, as the game sees it. */
static std::string start_card(const LauncherModel* m) {
    const char* card = m->s.memcard_path[0];
    if (card && card[0]) return card;
    const std::string folder = ai_card_folder(m);
    return folder.empty() ? std::string() : (std::filesystem::path(folder) / "card1.mcd").string();
}

static void start_read_saves(const LauncherModel* m) {
    g_start_saves.assign(15, moa_story_save{});
    const std::string card = start_card(m);
    const int n = card.empty() ? -1 : moa_memcard_list_story(card.c_str(), g_start_saves.data(), 15);
    g_start_saves_ok = n >= 0;
    g_start_saves.resize(n > 0 ? (size_t)n : 0);
}

static void open_start_page(LauncherModel* m) {
    (void)m;
    full_boot_read();
    g_start_page = true;
    g_start_list = false;
    g_state.cursor[PAGE_START] = 0;
    g_state.note[0] = '\0';
}

static void quick_launch(LauncherModel* m, const char* mode, const std::string& save) {
    set_ai_battle(false);
    set_arena_run(false, "");
    set_env("PSX_MOA_START_SAVE", save.c_str());
    set_env("PSX_MOA_FULL_BOOT", g_full_boot ? "1" : "0");
    g_quick_start = mode;
    g_start_page = g_start_list = false;     /* back on the main menu afterwards */
    start_game(m);
}

static void start_load(LauncherModel* m, const moa_story_save& s) {
    std::vector<uint8_t> block(MOA_SAVE_BLOCK);
    const std::string card = start_card(m);
    if (!moa_memcard_read_story(card.c_str(), s.slot, block.data())) {
        note("THAT SAVE COULD NOT BE READ");
        return;
    }
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) { note("NO TEMPORARY FOLDER"); return; }
    const std::string path = (dir / "moa-start-save.bin").string();
    std::FILE* f = std::fopen(path.c_str(), "wb");
    const bool ok = f && std::fwrite(block.data(), 1, block.size(), f) == block.size();
    if (f && std::fclose(f) != 0) { note("THAT SAVE COULD NOT BE READ"); return; }
    if (!ok) { note("THAT SAVE COULD NOT BE READ"); return; }
    quick_launch(m, "load", path);
}

static void draw_start_list(LauncherModel* m, const Gfx& g, const Input& in) {
    const int count = (int)g_start_saves.size();
    const int visible = 10;
    move_cursor(PAGE_START_LOAD, count, in, visible);
    int& cur = g_state.cursor[PAGE_START_LOAD];
    header(g, "LOAD GAME", "MEMORY CARD 1");
    panel(g, 24.0f, 76.0f, 592.0f, 290.0f, "SELECT DATA");
    int activate = -1;
    if (count == 0) {
        text(g, 38.0f, 94.0f, 11.0f, kDim,
             g_start_saves_ok ? "NO SCENARIO DATA ON MEMORY CARD 1."
                              : "MEMORY CARD 1 COULD NOT BE READ.", 560.0f);
    }
    const int top = g_state.top[PAGE_START_LOAD];
    for (int i = top; i < count && i < top + visible; ++i) {
        const moa_story_save& s = g_start_saves[(size_t)i];
        char label[64], value[48];
        std::snprintf(label, sizeof label, "%-8s  %s", upper(s.pilot).c_str(), upper(s.ac).c_str());
        std::snprintf(value, sizeof value, "%u C", (unsigned)s.money);
        bool hovered = false;
        if (row(g, 25.0f, 90.0f + kRowH * (i - top), 590.0f, i == cur, true, label, value,
                false, false, &hovered))
            activate = i;
        if (hovered && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f))
            cur = i;
    }
    if (count > 0) {
        const moa_story_save& s = g_start_saves[(size_t)cur];
        char buf[96];
        panel(g, 24.0f, 374.0f, 592.0f, 44.0f, "DATA");
        std::snprintf(buf, sizeof buf, "BLOCK %d   PARTS OWNED %d   %s", s.slot, s.owned, s.file);
        text(g, 38.0f, 390.0f, 11.0f, kText, buf);
    }
    footer(g, "A / ENTER  LOAD      D-PAD  MOVE      B / ESC  BACK");
    if (in.back) { g_start_list = false; return; }
    if (in.confirm && count > 0) activate = cur;
    if (activate >= 0) start_load(m, g_start_saves[(size_t)activate]);
}

static void draw_start_page(LauncherModel* m, const Gfx& g, const Input& in) {
    if (g_start_list) { draw_start_list(m, g, in); return; }
    enum { S_LOAD = 0, S_NEW, S_LAUNCH, S_ROWS };
    move_cursor(PAGE_START, S_ROWS, in, S_ROWS);
    int& cur = g_state.cursor[PAGE_START];
    header(g, "START GAME", "MASTER OF ARENA");
    panel(g, 24.0f, 76.0f, 592.0f, 96.0f, "SELECT");
    static const char* const kLabels[S_ROWS] = { "LOAD GAME", "NEW GAME", "LAUNCH GAME" };
    int activate = -1;
    for (int i = 0; i < S_ROWS; ++i) {
        bool hovered = false;
        const float y = 90.0f + (kRowH + 2.0f) * i;
        if (row(g, 25.0f, y, 590.0f, i == cur, true, kLabels[i], nullptr,
                i == S_LAUNCH, g_full_boot, &hovered))
            activate = i;
        if (i == S_LAUNCH)
            text_right(g, 25.0f + 590.0f - 60.0f, y + 7.0f, 10.0f, kDim, "FULL BOOT");
        if (hovered && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f))
            cur = i;
    }
    static const char* const kInfo[S_ROWS] = {
        "Choose Scenario data on memory card 1 and go straight to the hub with it "
        "loaded: no title screen and no memory card screens. Saving in the game "
        "still goes to memory card 1.",
        "Start a new Scenario game at the name entry, without the title screen.",
        "Start at the title screen. With FULL BOOT off the logos and intro movie are "
        "skipped; switch it on (LEFT / RIGHT) to see the game's whole boot.",
    };
    panel(g, 24.0f, 190.0f, 592.0f, 120.0f, "INFO");
    text(g, 38.0f, 206.0f, 11.0f, kHi, kLabels[cur]);
    text(g, 38.0f, 228.0f, 11.0f, kText, kInfo[cur], 560.0f);
    footer(g, cur == S_LAUNCH ? "A / ENTER  START      LEFT / RIGHT  FULL BOOT      B / ESC  BACK"
                              : "A / ENTER  SELECT      D-PAD  MOVE      B / ESC  BACK");
    if (in.back) { g_start_page = false; return; }
    if (cur == S_LAUNCH && (in.left || in.right)) full_boot_toggle();
    if (in.confirm) activate = cur;
    if (activate < 0) return;
    cur = activate;
    switch (activate) {
        case S_LOAD:
            start_read_saves(m);
            g_start_list = true;
            g_state.cursor[PAGE_START_LOAD] = 0;
            g_state.top[PAGE_START_LOAD] = 0;
            break;
        case S_NEW:    quick_launch(m, "new", std::string()); break;
        case S_LAUNCH: quick_launch(m, "title", std::string()); break;
    }
}
