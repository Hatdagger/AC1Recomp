/* ---- Arena Run page -------------------------------------------------------------
 *
 * The roguelike mode: NEW RUN, CONTINUE and SEED. The run is made here
 * (games/master-of-arena/moa_arena_run.c) and kept in a file beside memory
 * card 1; the game reads it through PSX_MOA_ARENA_RUN / PSX_MOA_RUN_FILE.
 * Included from moa_menu.inc.
 */
static uint64_t g_run_seed = 0;
static bool g_run_seed_set = false;      /* the player chose this seed */
static moa_run_state g_run;
static bool g_run_live = false;
static std::string g_run_status;
static char g_run_pilot[17] = "";
static char g_run_ac[17] = "";             /* optional: blank is fine */
enum { R_NEW = 0, R_CONTINUE, R_PILOT, R_AC, R_SEED, R_ROWS };

/* The run's names in the builder's editor (typed, or picked letter by letter). */
static void run_names_lend(bool on) {
    g_name_alt[1] = on ? g_run_ac : nullptr;
    g_name_alt[2] = on ? g_run_pilot : nullptr;
    if (!on) g_name_edit = 0;
}

static std::string run_file(const LauncherModel* m) {
    char p[1024];
    moa_run_path(m->s.memcard_path[0], p, sizeof p);
    return p;
}

static std::string run_card(const LauncherModel* m) {
    char p[1100];
    moa_run_card_path(m->s.memcard_path[0], p, sizeof p);
    return p;
}

/* The run plays from its own card in slot 1 (PSX_MEMCARD1_SESSION). */
static void set_arena_run(bool on, const std::string& path, const std::string& card = std::string()) {
    set_env("PSX_MOA_ARENA_RUN", on ? "1" : "0");
    set_env("PSX_MOA_RUN_FILE", on ? path.c_str() : "");
    set_env("PSX_MEMCARD1_SESSION", on ? card.c_str() : "");
}

static void open_run_page(LauncherModel* m) {
    g_state.run_page = true;
    g_state.cursor[PAGE_RUN] = 0;
    g_state.note[0] = '\0';
    g_run_live = moa_run_load(run_file(m).c_str(), &g_run) &&
                 std::filesystem::exists(std::filesystem::u8path(run_card(m)));
    if (g_run_live && !g_run_pilot[0]) {
        std::snprintf(g_run_pilot, sizeof g_run_pilot, "%s", g_run.pilot);
        std::snprintf(g_run_ac, sizeof g_run_ac, "%s", g_run.ac);
    }
    g_run_status.clear();
    run_names_lend(true);
    if (!g_run_seed_set) {
        uint64_t s = (uint64_t)std::time(nullptr) ^ ((uint64_t)std::rand() << 20);
        g_run_seed = moa_run_next(&s);
    }
}

static void new_run(LauncherModel* m) {
    const std::string disc = game_disc(m);
    if (disc.empty()) { note("SELECT YOUR MASTER OF ARENA DISC FIRST"); return; }
    if (!g_run_pilot[0]) {
        g_run_status = "ENTER A PILOT NAME FIRST.";
        g_state.cursor[PAGE_RUN] = R_PILOT;
        return;
    }
    static moa_parts parts;
    if (!moa_parts_load(disc.c_str(), &parts)) {
        g_run_status = "THE PART TABLES COULD NOT BE READ FROM YOUR DISC.";
        return;
    }
    const int pp = phantasma_installed(m) ? 1 : 0;
    moa_run_source src[16];
    const int n = moa_run_sources_load(disc.c_str(), pp, src, 16);
    if (n == 0 || !moa_run_new(&parts, src, n, pp, g_run_seed, &g_run)) {
        g_run_status = "A RUN COULD NOT BE MADE FROM YOUR DISC.";
        return;
    }
    std::snprintf(g_run.pilot, sizeof g_run.pilot, "%s", g_run_pilot);
    std::snprintf(g_run.ac, sizeof g_run.ac, "%s", g_run_ac);
    const std::string path = run_file(m);
    if (!moa_run_save(path.c_str(), &g_run) ||
        !moa_run_write_card(m->s.memcard_path[0], &g_run)) {
        g_run_status = "THE RUN COULD NOT BE WRITTEN BESIDE YOUR MEMORY CARD.";
        return;
    }
    g_run_live = true;
    g_run_seed_set = false;            /* the next run gets a fresh seed */
    run_names_lend(false);
    set_ai_battle(false);
    set_arena_run(true, path, run_card(m));
    start_game(m);
}

static void continue_run(LauncherModel* m) {
    if (!g_run_live) return;
    run_names_lend(false);
    set_ai_battle(false);
    set_arena_run(true, run_file(m), run_card(m));
    start_game(m);
}

static void draw_run_page(LauncherModel* m, const Gfx& g, const Input& in) {
    if (!g_name_edit) move_cursor(PAGE_RUN, R_ROWS, in, R_ROWS);
    int& cur = g_state.cursor[PAGE_RUN];
    header(g, "ARENA RUN", "50 OPPONENTS  ONE LIFE");
    panel(g, 24.0f, 76.0f, 592.0f, 166.0f, "SELECT");
    char seed[17];
    moa_run_seed_text(g_run_seed, seed);
    char cont[48] = "";
    if (g_run_live) std::snprintf(cont, sizeof cont, "%s  RANK %d", upper(g_run.pilot).c_str(), 50 - g_run.wins);
    const std::string pilot = name_shown(2), ac = name_shown(1);
    struct { const char* label; std::string value; bool on; } rows[R_ROWS] = {
        { "NEW RUN", g_run_live ? "ENDS THE CURRENT RUN" : "", true },
        { "CONTINUE", g_run_live ? cont : "NO RUN", g_run_live },
        { "PILOT NAME", g_name_edit == 2 ? pilot : (g_run_pilot[0] ? upper(g_run_pilot) : "REQUIRED"), true },
        { "AC NAME", g_name_edit == 1 ? ac : (g_run_ac[0] ? upper(g_run_ac) : "OPTIONAL"), true },
        { "SEED", seed, true },
    };
    int activate = -1;
    for (int i = 0; i < R_ROWS; ++i) {
        bool hovered = false;
        if (row(g, 25.0f, 90.0f + (kRowH + 2.0f) * i, 590.0f, cur == i, rows[i].on, rows[i].label,
                rows[i].value.c_str(), false, false, &hovered) && !g_name_edit)
            activate = i;
        if (hovered && !g_name_edit) cur = i;
    }

    panel(g, 24.0f, 248.0f, 592.0f, 168.0f, "RULES");
    text(g, 38.0f, 264.0f, 11.0f, kText,
         "A random legal AC and one arena of 50 opponents drawn from every arena, "
         "easiest first. Each win pays 1 P to spend in the shop. There is no "
         "leaving a fight, and one loss ends the run. The run is played from its "
         "own memory card; your saves are not touched.", 560.0f);
    text(g, 38.0f, 340.0f, 11.0f, kDim, "PROJECT PHANTASMA");
    text(g, 220.0f, 340.0f, 11.0f, phantasma_installed(m) ? kOn : kDim,
         phantasma_installed(m) ? "ITS OPPONENTS ARE IN THE DRAW" : "NOT INSTALLED (SEE EXTRAS)");
    if (g_name_edit)
        text(g, 38.0f, 362.0f, 10.0f, kHi,
             "TYPE, OR UP / DOWN  LETTER   LEFT / RIGHT  MOVE   X  DELETE   A  DONE", 560.0f);
    else if (cur == R_SEED)
        text(g, 38.0f, 362.0f, 10.0f, kDim,
             "Select for a new random seed. The same seed always makes the same AC and lineup.", 560.0f);
    if (!g_run_status.empty()) text(g, 38.0f, 384.0f, 11.0f, kWarn, g_run_status.c_str(), 560.0f);
    footer(g, "A / ENTER  SELECT      B / ESC  BACK");

    if (g_name_edit) { name_input(in); return; }
    if (in.back) { run_names_lend(false); g_state.run_page = false; return; }
    if (in.confirm) activate = cur;
    switch (activate) {
    case R_NEW: new_run(m); break;
    case R_CONTINUE: if (g_run_live) continue_run(m); break;
    case R_PILOT: case R_AC:
        g_name_edit = activate == R_PILOT ? 2 : 1;
        g_name_pos = (int)std::strlen(name_buf(g_name_edit));
        g_run_status.clear();
        break;
    case R_SEED:
        g_run_seed = moa_run_next(&g_run_seed);
        g_run_seed_set = true;
        break;
    }
}
