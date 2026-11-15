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

static std::string run_file(const LauncherModel* m) {
    char p[1024];
    moa_run_path(m->s.memcard_path[0], p, sizeof p);
    return p;
}

static void set_arena_run(bool on, const std::string& path) {
    set_env("PSX_MOA_ARENA_RUN", on ? "1" : "0");
    set_env("PSX_MOA_RUN_FILE", on ? path.c_str() : "");
}

static void open_run_page(LauncherModel* m) {
    g_state.run_page = true;
    g_state.cursor[PAGE_RUN] = 0;
    g_state.note[0] = '\0';
    g_run_live = moa_run_load(run_file(m).c_str(), &g_run);
    g_run_status.clear();
    if (!g_run_seed_set) {
        uint64_t s = (uint64_t)std::time(nullptr) ^ ((uint64_t)std::rand() << 20);
        g_run_seed = moa_run_next(&s);
    }
}

static void new_run(LauncherModel* m) {
    const std::string disc = game_disc(m);
    if (disc.empty()) { note("SELECT YOUR MASTER OF ARENA DISC FIRST"); return; }
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
    const std::string path = run_file(m);
    if (!moa_run_save(path.c_str(), &g_run)) {
        g_run_status = "THE RUN FILE COULD NOT BE WRITTEN BESIDE YOUR MEMORY CARD.";
        return;
    }
    g_run_live = true;
    g_run_seed_set = false;            /* the next run gets a fresh seed */
    set_ai_battle(false);
    set_arena_run(true, path);
    start_game(m);
}

static void continue_run(LauncherModel* m) {
    if (!g_run_live) return;
    set_ai_battle(false);
    set_arena_run(true, run_file(m));
    start_game(m);
}

static void draw_run_page(LauncherModel* m, const Gfx& g, const Input& in) {
    move_cursor(PAGE_RUN, 3, in, 3);
    int& cur = g_state.cursor[PAGE_RUN];
    header(g, "ARENA RUN", "50 OPPONENTS  ONE LIFE");
    panel(g, 24.0f, 76.0f, 592.0f, 96.0f, "SELECT");
    char seed[17];
    moa_run_seed_text(g_run_seed, seed);
    char cont[48] = "";
    if (g_run_live) std::snprintf(cont, sizeof cont, "RANK %d  %d P", 50 - g_run.wins, g_run.points);
    bool hovered[3] = { false, false, false };
    const bool c0 = row(g, 25.0f, 90.0f, 590.0f, cur == 0, true, "NEW RUN",
                        g_run_live ? "ENDS THE CURRENT RUN" : "", false, false, &hovered[0]);
    const bool c1 = row(g, 25.0f, 90.0f + kRowH + 2.0f, 590.0f, cur == 1, g_run_live,
                        "CONTINUE", g_run_live ? cont : "NO RUN", false, false, &hovered[1]);
    const bool c2 = row(g, 25.0f, 90.0f + 2.0f * (kRowH + 2.0f), 590.0f, cur == 2, true,
                        "SEED", seed, false, false, &hovered[2]);
    for (int i = 0; i < 3; ++i) if (hovered[i]) cur = i;

    panel(g, 24.0f, 178.0f, 592.0f, 238.0f, "RULES");
    text(g, 38.0f, 194.0f, 11.0f, kText,
         "A random legal AC and one arena of 50 opponents drawn from every arena, "
         "easiest first. Each win pays 1 P to spend in the shop. There is no "
         "leaving a fight, and one loss ends the run. The hub keeps only ARENA, "
         "SHOP, GARAGE and SYSTEM.", 560.0f);
    text(g, 38.0f, 282.0f, 11.0f, kDim, "PROJECT PHANTASMA");
    text(g, 220.0f, 282.0f, 11.0f, phantasma_installed(m) ? kOn : kDim,
         phantasma_installed(m) ? "ITS OPPONENTS ARE IN THE DRAW" : "NOT INSTALLED (SEE EXTRAS)");
    if (cur == 2)
        text(g, 38.0f, 306.0f, 10.0f, kDim,
             "Select for a new random seed. The same seed always makes the same AC "
             "and lineup.", 560.0f);
    if (!g_run_status.empty()) text(g, 38.0f, 366.0f, 11.0f, kWarn, g_run_status.c_str(), 560.0f);
    footer(g, "A / ENTER  SELECT      B / ESC  BACK");

    if (in.back) { g_state.run_page = false; return; }
    if (c0 || (in.confirm && cur == 0)) new_run(m);
    else if ((c1 || (in.confirm && cur == 1)) && g_run_live) continue_run(m);
    else if (c2 || (in.confirm && cur == 2)) {
        g_run_seed = moa_run_next(&g_run_seed);
        g_run_seed_set = true;
    }
}
