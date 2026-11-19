/* ---- Rogue Arena page (Arena Run) -------------------------------------------------------------
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
static bool g_run_over = false;       /* the last run was lost: Retry */
static std::string g_run_status;
static char g_run_pilot[17] = "";
static char g_run_ac[17] = "";             /* optional: blank is fine */
enum { R_NEW = 0, R_CONTINUE, R_PILOT, R_AC, R_SEED, R_FAST, R_ROWS };
/* FAST BOOT: the game's direct boot (no title, LOAD GAME or card screens;
 * moa_arena_run_boot.c). On by default; OFF goes back to the menu presses,
 * the fallback if the direct boot ever fails. Not kept between launcher
 * sessions. */
static bool g_run_fast = true;

/* The one-time notice before the first run (kept from the demo for a feature
 * to come): once OK is pressed a marker file beside memory card 1 keeps it
 * from showing again. */
static const char* const kRunNotice =
    "Rogue Arena may use your in-game save data, such as your AC builds, "
    "names and run results, and may share it online with other players. "
    "Press OK to continue.";
static int g_run_notice_for = -1;        /* the row waiting on the notice; -1 = none */

static std::string run_notice_file(const LauncherModel* m) {
    char p[1100];
    moa_run_path(m->s.memcard_path[0], p, sizeof p);
    return std::string(p) + ".notice";
}

static bool run_notice_accepted(const LauncherModel* m) {
    return std::filesystem::exists(std::filesystem::u8path(run_notice_file(m)));
}

static void run_notice_accept(const LauncherModel* m) {
    FILE* f = std::fopen(run_notice_file(m).c_str(), "wb");
    if (f) { std::fputs("accepted\n", f); std::fclose(f); }
}

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
/* A run game was started: when the launcher shows the page again, it reads
 * the run file first (a loss there means RETRY). The launcher isn't drawn
 * during a game, so this can't wait to see the session end. */
static bool g_run_reread = false;
static bool g_run_next_poll = false;       /* look for a loss-screen pick once, back from the game */

static void set_arena_run(bool on, const std::string& path, const std::string& card = std::string()) {
    set_env("PSX_MOA_ARENA_RUN", on ? "1" : "0");
    set_env("PSX_MOA_RUN_FILE", on ? path.c_str() : "");
    set_env("PSX_MOA_RUN_DIRECT", on && g_run_fast ? "1" : "0");
    set_env("PSX_MEMCARD1_SESSION", on ? card.c_str() : "");
    /* The garage's part models: the run draws its prize screen pictures from them. */
    set_env("PSX_MOA_MODELS_DIR", on ? asset("acmodels").c_str() : "");
    if (on) g_run_reread = g_run_next_poll = true;
}

static void open_run_page(LauncherModel* m) {
    g_state.run_page = true;
    g_state.cursor[PAGE_RUN] = 0;
    g_state.note[0] = '\0';
    const int got = moa_run_load(run_file(m).c_str(), &g_run);
    g_run_over = got == 2;
    g_run_live = got == 1 &&
                 std::filesystem::exists(std::filesystem::u8path(run_card(m)));
    if ((g_run_live || g_run_over) && !g_run_pilot[0]) {
        std::snprintf(g_run_pilot, sizeof g_run_pilot, "%s", g_run.run.pilot);
        std::snprintf(g_run_ac, sizeof g_run_ac, "%s", g_run.run.ac);
    }
    g_run_status.clear();
    run_names_lend(true);
    if (!g_run_seed_set) {
        uint64_t s = (uint64_t)std::time(nullptr) ^ ((uint64_t)std::rand() << 20);
        g_run_seed = moa_run_next(&s);
    }
}

static bool start_run(LauncherModel* m);

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
    (void)moa_run_sources_mark(disc.c_str(), &parts, src, n);   /* KARASAWA users: top 25 */
    if (n == 0 || !moa_run_new(&parts, src, n, pp, g_run_seed, &g_run)) {
        g_run_status = "A RUN COULD NOT BE MADE FROM YOUR DISC.";
        return;
    }
    g_run.lineup.menu_lba = moa_disc_file_lba(disc.c_str(), "GG/MS/MENU_TIM.T");
    if (!g_run.lineup.menu_lba) {
        g_run_status = "THE ARENA DATA COULD NOT BE FOUND ON YOUR DISC.";
        return;
    }
    std::snprintf(g_run.run.pilot, sizeof g_run.run.pilot, "%s", g_run_pilot);
    std::snprintf(g_run.run.ac, sizeof g_run.run.ac, "%s", g_run_ac);
    if (!start_run(m)) return;
    g_run_seed_set = false;            /* the next run gets a fresh seed */
}

/* Write the run and its card, then start the game on it. */
static bool start_run(LauncherModel* m) {
    const std::string path = run_file(m);
    if (!moa_run_save(path.c_str(), &g_run) ||
        !moa_run_write_card(m->s.memcard_path[0], &g_run)) {
        g_run_status = "THE RUN COULD NOT BE WRITTEN BESIDE YOUR MEMORY CARD.";
        return false;
    }
    g_run_live = true;
    g_run_over = false;
    run_names_lend(false);
    set_ai_battle(false);
    set_arena_run(true, path, run_card(m));
    start_game(m);
    return true;
}

/* Same arena (with the ghost of the losing AC), a new AC, 0 P. */
static void retry_run(LauncherModel* m) {
    const std::string disc = game_disc(m);
    if (disc.empty()) { note("SELECT YOUR MASTER OF ARENA DISC FIRST"); return; }
    if (!g_run_pilot[0]) {
        g_run_status = "ENTER A PILOT NAME FIRST.";
        g_state.cursor[PAGE_RUN] = R_PILOT;
        return;
    }
    static moa_parts parts;
    if (!moa_parts_load(disc.c_str(), &parts) || !moa_run_retry(&parts, &g_run)) {
        g_run_status = "THE RUN COULD NOT BE RETRIED.";
        return;
    }
    /* The new attempt flies under the names on the page (the ghost of the
     * last attempt keeps the old ones). */
    std::snprintf(g_run.run.pilot, sizeof g_run.run.pilot, "%s", g_run_pilot);
    std::snprintf(g_run.run.ac, sizeof g_run.run.ac, "%s", g_run_ac);
    start_run(m);
}

static void continue_run(LauncherModel* m) {
    if (!g_run_live) return;
    run_names_lend(false);
    set_ai_battle(false);
    set_arena_run(true, run_file(m), run_card(m));
    start_game(m);
}

/* The game's loss screen leaves its pick in "<run file>.next": "retry" (the
 * RETRY row) or "new" (NEW RUN with a fresh seed, same names). Read once and
 * removed, so it never repeats. */
static std::string run_next_take(const LauncherModel* m) {
    const std::string p = run_file(m) + ".next";
    std::string what;
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return what;
    char buf[16] = "";
    if (std::fgets(buf, sizeof buf, f)) what = buf;
    std::fclose(f);
    std::remove(p.c_str());
    while (!what.empty() && (what.back() == '\n' || what.back() == '\r' || what.back() == ' ')) what.pop_back();
    return what;
}

static bool run_next_waiting(const LauncherModel* m) {
    return std::filesystem::exists(std::filesystem::u8path(run_file(m) + ".next"));
}

static void draw_run_page(LauncherModel* m, const Gfx& g, const Input& in) {
    if (g_run_reread) {
        g_run_reread = false;
        open_run_page(m);
        g_state.cursor[PAGE_RUN] = R_CONTINUE;
        const std::string next = run_next_take(m);
        if (next == "retry" && g_run_over) { retry_run(m); return; }
        if (next == "new") { g_state.cursor[PAGE_RUN] = R_NEW; new_run(m); return; }
    }
    if (!g_name_edit) move_cursor(PAGE_RUN, R_ROWS, in, R_ROWS);
    int& cur = g_state.cursor[PAGE_RUN];
    header(g, "ROGUE ARENA", "49 OPPONENTS  ONE LIFE");
    panel(g, 24.0f, 76.0f, 592.0f, 194.0f, "SELECT");
    char seed[17];
    moa_run_seed_text(g_run_seed, seed);
    char cont[48] = "";
    if (g_run_live)
        std::snprintf(cont, sizeof cont, "%s  RANK %d", upper(g_run.run.pilot).c_str(), 50 - g_run.run.wins);
    else if (g_run_over)
        std::snprintf(cont, sizeof cont, "LOST AT RANK %d  SAME ARENA, NEW AC", 50 - g_run.run.wins);
    const std::string pilot = name_shown(2), ac = name_shown(1);
    struct { const char* label; std::string value; bool on; } rows[R_ROWS] = {
        { "NEW RUN", g_run_live ? "ENDS THE CURRENT RUN" : "", true },
        { g_run_over ? "RETRY" : "CONTINUE", (g_run_live || g_run_over) ? cont : "NO RUN",
          g_run_live || g_run_over },
        { "PILOT NAME", g_name_edit == 2 ? pilot : (g_run_pilot[0] ? upper(g_run_pilot) : "REQUIRED"), true },
        { "AC NAME", g_name_edit == 1 ? ac : (g_run_ac[0] ? upper(g_run_ac) : "OPTIONAL"), true },
        { "SEED", seed, true },
        { "FAST BOOT", g_run_fast ? "ON" : "OFF", true },
    };
    int activate = -1;
    for (int i = 0; i < R_ROWS; ++i) {
        bool hovered = false;
        if (row(g, 25.0f, 90.0f + (kRowH + 2.0f) * i, 590.0f, cur == i, rows[i].on, rows[i].label,
                rows[i].value.c_str(), false, false, &hovered) && !g_name_edit && g_run_notice_for < 0)
            activate = i;
        if (hovered && !g_name_edit && g_run_notice_for < 0) cur = i;
    }

    panel(g, 24.0f, 276.0f, 592.0f, 160.0f, "RULES");
    text(g, 38.0f, 292.0f, 11.0f, kText,
         "A random legal AC and 49 opponents from every arena, easiest first; you "
         "start at the bottom. Each win pays 1 P for the shop, and wins 1, 3, 6, 9, "
         "12, 18 and 24 give a prize pick. No leaving a fight; one loss ends the "
         "run. It plays from its own memory card; your saves are not touched.", 560.0f);
    text(g, 38.0f, 336.0f, 9.0f, kDim, kRunNotice, 560.0f);
    text(g, 38.0f, 368.0f, 11.0f, kDim, "PROJECT PHANTASMA");
    text(g, 220.0f, 368.0f, 11.0f, phantasma_installed(m) ? kOn : kDim,
         phantasma_installed(m) ? "ITS OPPONENTS ARE IN THE DRAW" : "NOT INSTALLED (SEE EXTRAS)");
    if (!g_name_edit && cur == R_SEED)
        text(g, 38.0f, 390.0f, 10.0f, kDim,
             "Select for a new random seed. The same seed always makes the same AC and lineup.", 560.0f);
    if (!g_name_edit && cur == R_FAST)
        text(g, 38.0f, 390.0f, 10.0f, kDim,
             "Test: skip the title and load screens and go straight to the Arena list.", 560.0f);
    if (!g_run_status.empty()) text(g, 38.0f, 412.0f, 11.0f, kWarn, g_run_status.c_str(), 560.0f);
    if (g_run_notice_for >= 0) {
        panel(g, 120.0f, 150.0f, 400.0f, 130.0f, "ROGUE ARENA");
        text(g, 134.0f, 168.0f, 11.0f, kText, kRunNotice, 372.0f);
        text(g, 134.0f, 252.0f, 10.0f, kText, "A  OK      B  BACK");
    }
    footer(g, g_run_notice_for >= 0 ? "A / ENTER  OK      B / ESC  BACK"
                                     : "A / ENTER  SELECT      B / ESC  BACK");

    if (g_name_edit) { name_input(g, in); return; }
    if (g_run_notice_for >= 0) {
        if (in.back) { g_run_notice_for = -1; return; }
        if (!in.confirm) return;
        run_notice_accept(m);
        activate = g_run_notice_for;
        g_run_notice_for = -1;
    } else {
        if (in.back) { run_names_lend(false); g_state.run_page = false; return; }
        if (in.confirm) activate = cur;
        /* A run is about to start: the notice first, once. */
        const bool starts = activate == R_NEW || (activate == R_CONTINUE && (g_run_live || g_run_over));
        if (starts && !run_notice_accepted(m)) { g_run_notice_for = activate; return; }
    }
    switch (activate) {
    case R_NEW: new_run(m); break;
    case R_CONTINUE:
        if (g_run_live) continue_run(m);
        else if (g_run_over) retry_run(m);
        break;
    case R_PILOT: case R_AC:
        g_name_edit = activate == R_PILOT ? 2 : 1;
        g_name_pos = (int)std::strlen(name_buf(g_name_edit));
        g_run_status.clear();
        break;
    case R_SEED:
        g_run_seed = moa_run_next(&g_run_seed);
        g_run_seed_set = true;
        break;
    case R_FAST:
        g_run_fast = !g_run_fast;
        break;
    }
}
