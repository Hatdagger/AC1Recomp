/* ---- Link Versus page ---------------------------------------------------------
 *
 * Two game sessions on one PC, one per monitor, joined by an emulated link
 * cable (psxrecomp/runtime/src/sio1.c). This page is the card screen: each
 * player picks their own card for slot 1 and their sound output; slot 2 is
 * always that player's QUICK START card (games/master-of-arena/moa_link.c),
 * kept between launches. A simple save manager copies or deletes saves.
 *
 * START LINK VERSUS starts this process as session 1 and asks it to start
 * session 2 (PSX_LINK_SPAWN; runtime main.cpp link_spawn_partner) with
 * player 2's cards and sound. Included from moa_menu.inc after the run page.
 */
#include "../../../../../games/master-of-arena/moa_link.h"

enum { LV_SLOT1 = 0, LV_SLOT2, LV_RESET, LV_SOUND, LV_ROWS_PER };
static const int kLinkRows = LV_ROWS_PER * 2 + 1;   /* + START */

struct LinkPlayer {
    std::string card;      /* slot 1: a .mcd path; empty = NEW CARD */
    std::string audio;     /* output device name; empty = system default */
};
static LinkPlayer g_lv[2];
static bool g_lv_loaded = false;
static std::vector<std::string> g_lv_cards;      /* .mcd files in the card folder */
static std::vector<std::string> g_lv_audio;      /* output device names */
static std::string g_lv_status;
static bool g_lv_status_bad = false;
static int  g_lv_reset_ask = -1;                 /* player asked to confirm RESET */

/* The sound test: which player's output is playing, and until when. */
static int    g_lv_test_player = -1;
static double g_lv_test_until = 0.0;
#if defined(LNG_SDL3)
static SDL_AudioStream* g_lv_test_stream = nullptr;
#else
static SDL_AudioDeviceID g_lv_test_dev = 0;
#endif

/* Save manager. */
static bool g_lv_mgr = false;
static std::string g_lv_mgr_card;                /* the card being shown */
static std::string g_lv_mgr_label;
static std::vector<moa_card_save> g_lv_mgr_saves;
static int  g_lv_mgr_cur = 0;
static int  g_lv_mgr_action = -1;                /* -1 list; 0 COPY, 1 DELETE, 2 BACK */
static int  g_lv_mgr_target = 0;
static bool g_lv_mgr_confirm = false;

static std::string lv_folder(const LauncherModel* m) { return ai_card_folder(m); }

static std::string lv_dir(const LauncherModel* m) {
    const std::string f = lv_folder(m);
    return f.empty() ? std::string() : (std::filesystem::path(f) / "link-versus").string();
}

static std::string lv_quickstart(const LauncherModel* m, int p) {
    const std::string d = lv_dir(m);
    return d.empty() ? std::string()
                     : (std::filesystem::path(d) / (p ? "quickstart-p2.mcd" : "quickstart-p1.mcd")).string();
}

static std::string lv_choices_file(const LauncherModel* m) {
    const std::string d = lv_dir(m);
    return d.empty() ? std::string() : (std::filesystem::path(d) / "link-versus.txt").string();
}

static std::string lv_base(const std::string& path) {
    return path.empty() ? std::string("NEW CARD")
                        : upper(std::filesystem::u8path(path).filename().u8string().c_str());
}

static void lv_save_choices(const LauncherModel* m) {
    const std::string f = lv_choices_file(m);
    if (f.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(lv_dir(m)), ec);
    FILE* out = std::fopen(f.c_str(), "wb");
    if (!out) return;
    for (int p = 0; p < 2; ++p)
        std::fprintf(out, "p%d.card=%s\np%d.audio=%s\n", p + 1, g_lv[p].card.c_str(),
                     p + 1, g_lv[p].audio.c_str());
    std::fclose(out);
}

static void lv_load_choices(const LauncherModel* m) {
    g_lv_loaded = true;
    const char* card1 = m->s.memcard_path[0];
    g_lv[0].card = card1 && card1[0] ? card1
                 : (lv_folder(m).empty() ? std::string()
                    : (std::filesystem::path(lv_folder(m)) / "card1.mcd").string());
    g_lv[1].card = lv_folder(m).empty() ? std::string()
                 : (std::filesystem::path(lv_folder(m)) / "card2.mcd").string();
    if (!std::filesystem::exists(std::filesystem::u8path(g_lv[1].card))) g_lv[1].card.clear();
    FILE* f = std::fopen(lv_choices_file(m).c_str(), "rb");
    if (!f) return;
    char line[1200];
    while (std::fgets(line, sizeof line, f)) {
        size_t n = std::strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        for (int p = 0; p < 2; ++p) {
            char key[16];
            std::snprintf(key, sizeof key, "p%d.card=", p + 1);
            if (std::strncmp(line, key, std::strlen(key)) == 0) g_lv[p].card = line + std::strlen(key);
            std::snprintf(key, sizeof key, "p%d.audio=", p + 1);
            if (std::strncmp(line, key, std::strlen(key)) == 0) g_lv[p].audio = line + std::strlen(key);
        }
    }
    std::fclose(f);
}

static void lv_scan(const LauncherModel* m) {
    namespace fs = std::filesystem;
    g_lv_cards.clear();
    const std::string folder = lv_folder(m);
    std::error_code ec;
    if (!folder.empty())
        for (fs::directory_iterator it(fs::u8path(folder), ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code fec;
            if (!it->is_regular_file(fec)) continue;
            if (upper(it->path().extension().u8string().c_str()) == ".MCD")
                g_lv_cards.push_back(it->path().u8string());
        }
    std::sort(g_lv_cards.begin(), g_lv_cards.end());
    for (int p = 0; p < 2; ++p)   /* a chosen card that was moved away: back to NEW CARD */
        if (!g_lv[p].card.empty() && !fs::exists(fs::u8path(g_lv[p].card), ec) &&
            g_lv[p].card != (folder.empty() ? std::string() : (fs::path(folder) / "card1.mcd").string()))
            g_lv[p].card.clear();

    g_lv_audio.clear();
    SDL_InitSubSystem(SDL_INIT_AUDIO);
#if defined(LNG_SDL3)
    int count = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count);
    for (int i = 0; ids && i < count; ++i)
        if (const char* name = SDL_GetAudioDeviceName(ids[i])) g_lv_audio.push_back(name);
    SDL_free(ids);
#else
    const int count = SDL_GetNumAudioDevices(0);
    for (int i = 0; i < count; ++i)
        if (const char* name = SDL_GetAudioDeviceName(i, 0)) g_lv_audio.push_back(name);
#endif
}

/* ---- sound test ---- */

static void lv_test_stop() {
#if defined(LNG_SDL3)
    if (g_lv_test_stream) { SDL_DestroyAudioStream(g_lv_test_stream); g_lv_test_stream = nullptr; }
#else
    if (g_lv_test_dev) { SDL_CloseAudioDevice(g_lv_test_dev); g_lv_test_dev = 0; }
#endif
    g_lv_test_player = -1;
}

/* A short chime on the player's output: one note for player 1, two for
 * player 2, so the ear can tell them apart too. */
static void lv_test_sound(int p) {
    lv_test_stop();
    const int rate = 48000;
    const int tone = rate / 6, gap = rate / 16;
    const int notes = p + 1;
    std::vector<int16_t> pcm((size_t)(notes * (tone + gap)) * 2, 0);
    for (int k = 0; k < notes; ++k) {
        const double freq = k ? 1318.5 : 880.0;
        for (int i = 0; i < tone; ++i) {
            const double env = std::min(1.0, i / 400.0) * std::max(0.0, 1.0 - (double)i / tone);
            const int16_t v = (int16_t)(std::sin(2.0 * 3.14159265358979 * freq * i / rate) * env * 9000.0);
            const size_t pos = (size_t)(k * (tone + gap) + i) * 2;
            pcm[pos] = pcm[pos + 1] = v;
        }
    }
#if defined(LNG_SDL3)
    SDL_AudioDeviceID target = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (!g_lv[p].audio.empty()) {
        int count = 0;
        SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count);
        for (int i = 0; ids && i < count; ++i) {
            const char* name = SDL_GetAudioDeviceName(ids[i]);
            if (name && g_lv[p].audio == name) { target = ids[i]; break; }
        }
        SDL_free(ids);
    }
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = rate;
    g_lv_test_stream = SDL_OpenAudioDeviceStream(target, &spec, nullptr, nullptr);
    if (!g_lv_test_stream) { note("THAT SOUND OUTPUT COULD NOT BE OPENED"); return; }
    SDL_PutAudioStreamData(g_lv_test_stream, pcm.data(), (int)(pcm.size() * sizeof(int16_t)));
    SDL_ResumeAudioStreamDevice(g_lv_test_stream);
#else
    SDL_AudioSpec want{}, have{};
    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    g_lv_test_dev = SDL_OpenAudioDevice(g_lv[p].audio.empty() ? nullptr : g_lv[p].audio.c_str(),
                                        0, &want, &have, 0);
    if (!g_lv_test_dev) { note("THAT SOUND OUTPUT COULD NOT BE OPENED"); return; }
    SDL_QueueAudio(g_lv_test_dev, pcm.data(), (Uint32)(pcm.size() * sizeof(int16_t)));
    SDL_PauseAudioDevice(g_lv_test_dev, 0);
#endif
    g_lv_test_player = p;
    g_lv_test_until = ImGui::GetTime() + 1.6;
}

/* ---- the page ---- */

static void set_link_env(bool on) {
    if (on) return;
    for (const char* e : { "PSX_LINK_CABLE", "PSX_LINK_SIDE", "PSX_LINK_SPAWN", "PSX_AUDIO_DEVICE",
                           "PSX_LINK_P2_CARD1", "PSX_LINK_P2_CARD2", "PSX_LINK_P2_AUDIO",
                           "PSX_MEMCARD1_SESSION", "PSX_MEMCARD2_SESSION" })
        set_env(e, "");
}

static void open_link_page(LauncherModel* m) {
    g_state.link_page = true;
    g_state.cursor[PAGE_LINK] = 0;
    g_lv_status.clear();
    g_lv_mgr = false;
    g_lv_reset_ask = -1;
    if (!g_lv_loaded) lv_load_choices(m);
    lv_scan(m);
    /* Each player's QUICK START card is ready (and shown) as soon as the page is. */
    for (int p = 0; p < 2; ++p) {
        char err[200] = "";
        int put = 0;
        const std::string qs = lv_quickstart(m, p);
        if (qs.empty() || !moa_link_quickstart_ensure(qs.c_str(), &put, err, sizeof err)) {
            g_lv_status = std::string("PLAYER ") + (p ? "2" : "1") + " QUICK START: " + upper(err);
            g_lv_status_bad = true;
        }
    }
}

static void close_link_page() {
    lv_test_stop();
    g_state.link_page = false;
}

/* A new empty card in the card folder: card<N>.mcd, the first unused N. */
static std::string lv_new_card(const LauncherModel* m, std::string* err) {
    const std::string folder = lv_folder(m);
    if (folder.empty()) { *err = "NO MEMORY CARD FOLDER"; return std::string(); }
    for (int n = 2; n < 1000; ++n) {
        const std::string p = (std::filesystem::path(folder) / ("card" + std::to_string(n) + ".mcd")).string();
        if (std::filesystem::exists(std::filesystem::u8path(p))) continue;
        if (g_lv[0].card == p || g_lv[1].card == p) continue;
        char e[200] = "";
        if (!moa_memcard_create(p.c_str(), e, sizeof e)) { *err = upper(e); return std::string(); }
        return p;
    }
    *err = "NO FREE CARD NAME";
    return std::string();
}

/* Slot 1 choices: every card in the folder the other player is not using,
 * then NEW CARD. */
static void lv_step_card(int p, int dir) {
    std::vector<std::string> choices;
    for (const std::string& c : g_lv_cards) if (c != g_lv[p ^ 1].card) choices.push_back(c);
    choices.push_back(std::string());
    int pos = 0;
    for (size_t i = 0; i < choices.size(); ++i) if (choices[i] == g_lv[p].card) pos = (int)i;
    const int n = (int)choices.size();
    g_lv[p].card = choices[(size_t)((pos + dir + n) % n)];
}

static void lv_step_audio(int p, int dir) {
    const int n = (int)g_lv_audio.size() + 1;
    int pos = 0;
    for (size_t i = 0; i < g_lv_audio.size(); ++i) if (g_lv_audio[i] == g_lv[p].audio) pos = (int)i + 1;
    pos = (pos + dir + n) % n;
    g_lv[p].audio = pos ? g_lv_audio[(size_t)pos - 1] : std::string();
    lv_test_sound(p);
}

static std::string lv_audio_label(int p) {
    if (g_lv[p].audio.empty()) return "SYSTEM DEFAULT";
    bool there = false;
    for (const std::string& a : g_lv_audio) if (a == g_lv[p].audio) there = true;
    return upper(g_lv[p].audio.c_str()) + (there ? "" : " (NOT CONNECTED)");
}

static void lv_open_manager(const std::string& card, const std::string& label) {
    g_lv_mgr = true;
    g_lv_mgr_card = card;
    g_lv_mgr_label = label;
    g_lv_mgr_cur = 0;
    g_lv_mgr_action = -1;
    g_lv_mgr_confirm = false;
    g_lv_mgr_saves.clear();
    moa_card_save list[15];
    const int n = moa_memcard_list_saves(card.c_str(), list, 15);
    for (int i = 0; i < n; ++i) g_lv_mgr_saves.push_back(list[i]);
}

static void start_link_versus(LauncherModel* m) {
    for (int p = 0; p < 2; ++p) {
        if (g_lv[p].card.empty()) {
            std::string err;
            const std::string made = lv_new_card(m, &err);
            if (made.empty()) { g_lv_status = "PLAYER " + std::to_string(p + 1) + " NEW CARD: " + err; g_lv_status_bad = true; return; }
            g_lv[p].card = made;
        }
        char err[200] = "";
        if (!moa_link_quickstart_ensure(lv_quickstart(m, p).c_str(), nullptr, err, sizeof err)) {
            g_lv_status = "PLAYER " + std::to_string(p + 1) + " QUICK START: " + upper(err);
            g_lv_status_bad = true;
            return;
        }
    }
    if (g_lv[0].card == g_lv[1].card) {
        g_lv_status = "THE TWO PLAYERS NEED DIFFERENT SLOT 1 CARDS";
        g_lv_status_bad = true;
        return;
    }
    lv_save_choices(m);
    lv_test_stop();
    char cable[40];
    std::snprintf(cable, sizeof cable, "lv-%ld-%u", (long)std::time(nullptr), (unsigned)(std::rand() & 0xFFFF));
    set_ai_battle(false);
    set_arena_run(false, std::string());
    set_env("PSX_LINK_CABLE", cable);
    set_env("PSX_LINK_SIDE", "1");
    set_env("PSX_LINK_SPAWN", "1");
    set_env("PSX_MEMCARD1_SESSION", g_lv[0].card.c_str());
    set_env("PSX_MEMCARD2_SESSION", lv_quickstart(m, 0).c_str());
    set_env("PSX_AUDIO_DEVICE", g_lv[0].audio.c_str());
    set_env("PSX_LINK_P2_CARD1", g_lv[1].card.c_str());
    set_env("PSX_LINK_P2_CARD2", lv_quickstart(m, 1).c_str());
    set_env("PSX_LINK_P2_AUDIO", g_lv[1].audio.c_str());
    start_game(m);
}

/* The save manager over the page. Returns after drawing; input is its own. */
static void draw_link_manager(LauncherModel* m, const Gfx& g, const Input& in) {
    panel(g, 40.0f, 70.0f, 560.0f, 350.0f, "SAVES");
    text(g, 54.0f, 84.0f, 11.0f, kHi, g_lv_mgr_label.c_str());
    char buf[200];
    const int free_blocks = moa_memcard_free_blocks(g_lv_mgr_card.c_str());
    std::snprintf(buf, sizeof buf, "%s   %d FREE BLOCK%s", lv_base(g_lv_mgr_card).c_str(),
                  free_blocks < 0 ? 0 : free_blocks, free_blocks == 1 ? "" : "S");
    text_right(g, 586.0f, 84.0f, 10.0f, kDim, buf);
    const int n = (int)g_lv_mgr_saves.size();
    if (n == 0) text(g, 54.0f, 112.0f, 11.0f, kDim, free_blocks < 0 ? "THIS CARD COULD NOT BE READ." : "NO SAVES ON THIS CARD.");
    for (int i = 0; i < n && i < 10; ++i) {
        const moa_card_save& s = g_lv_mgr_saves[(size_t)i];
        std::string label = s.moa ? (upper(s.ac) + "  /  " + upper(s.pilot)) : upper(s.title);
        if (s.quick_start) label += "  (QUICK START)";
        std::snprintf(buf, sizeof buf, "%d BLOCK%s", s.blocks, s.blocks == 1 ? "" : "S");
        row(g, 41.0f, 104.0f + kRowH * i, 558.0f, i == g_lv_mgr_cur && g_lv_mgr_action < 0, true,
            label.c_str(), buf, false, false, nullptr);
    }

    /* Copy targets: the four cards on this page, other than this one. */
    std::vector<std::pair<std::string, std::string>> targets;
    for (int p = 0; p < 2; ++p) {
        const std::string who = "PLAYER " + std::to_string(p + 1);
        if (!g_lv[p].card.empty() && g_lv[p].card != g_lv_mgr_card)
            targets.push_back({ g_lv[p].card, who + " SLOT 1 (" + lv_base(g_lv[p].card) + ")" });
        if (lv_quickstart(m, p) != g_lv_mgr_card)
            targets.push_back({ lv_quickstart(m, p), who + " SLOT 2 (QUICK START)" });
    }
    if (g_lv_mgr_target >= (int)targets.size()) g_lv_mgr_target = 0;

    if (g_lv_mgr_action >= 0) {
        panel(g, 120.0f, 300.0f, 400.0f, 110.0f, "THIS SAVE");
        const std::string copy = targets.empty() ? std::string("NO OTHER CARD")
                                                 : targets[(size_t)g_lv_mgr_target].second;
        const char* labels[3] = { "COPY TO", g_lv_mgr_confirm ? "DELETE: PRESS A AGAIN" : "DELETE", "BACK" };
        const std::string values[3] = { copy, "", "" };
        for (int i = 0; i < 3; ++i)
            row(g, 121.0f, 314.0f + kRowH * i, 398.0f, i == g_lv_mgr_action, i != 0 || !targets.empty(),
                labels[i], values[i].c_str(), false, false, nullptr);
    }
    footer(g, g_lv_mgr_action >= 0 ? "A  OK      LEFT / RIGHT  CARD      B  BACK"
                                   : "A  CHOOSE A SAVE      B  CLOSE");

    if (g_lv_mgr_action < 0) {
        if (in.back) { g_lv_mgr = false; return; }
        if (n > 0) {
            if (in.up) g_lv_mgr_cur = (g_lv_mgr_cur + n - 1) % n;
            if (in.down) g_lv_mgr_cur = (g_lv_mgr_cur + 1) % n;
            if (in.confirm) { g_lv_mgr_action = 0; g_lv_mgr_confirm = false; }
        }
        return;
    }
    if (in.back) { g_lv_mgr_action = -1; g_lv_mgr_confirm = false; return; }
    if (in.up)   { g_lv_mgr_action = (g_lv_mgr_action + 2) % 3; g_lv_mgr_confirm = false; }
    if (in.down) { g_lv_mgr_action = (g_lv_mgr_action + 1) % 3; g_lv_mgr_confirm = false; }
    if (g_lv_mgr_action == 0 && !targets.empty()) {
        const int t = (int)targets.size();
        if (in.left)  g_lv_mgr_target = (g_lv_mgr_target + t - 1) % t;
        if (in.right) g_lv_mgr_target = (g_lv_mgr_target + 1) % t;
    }
    if (!in.confirm) return;
    const moa_card_save s = g_lv_mgr_saves[(size_t)g_lv_mgr_cur];
    char err[200] = "";
    if (g_lv_mgr_action == 2) { g_lv_mgr_action = -1; return; }
    if (g_lv_mgr_action == 0 && !targets.empty()) {
        const std::string dst = targets[(size_t)g_lv_mgr_target].first;
        /* A card picked as NEW CARD is made now. */
        if (moa_memcard_free_blocks(dst.c_str()) < 0 && !std::filesystem::exists(std::filesystem::u8path(dst)))
            moa_memcard_create(dst.c_str(), err, sizeof err);
        if (moa_memcard_copy_save(g_lv_mgr_card.c_str(), s.slot, dst.c_str(), err, sizeof err))
            note("COPIED");
        else note(upper(err).c_str());
        g_lv_mgr_action = -1;
        return;
    }
    if (g_lv_mgr_action == 1) {
        if (!g_lv_mgr_confirm) { g_lv_mgr_confirm = true; return; }
        if (moa_memcard_delete_save(g_lv_mgr_card.c_str(), s.slot, err, sizeof err)) note("DELETED (A BACKUP OF THE CARD IS KEPT)");
        else note(upper(err).c_str());
        lv_open_manager(g_lv_mgr_card, g_lv_mgr_label);
    }
}

static void draw_link_page(LauncherModel* m, const Gfx& g, const Input& in) {
    if (g_lv_test_player >= 0 && ImGui::GetTime() > g_lv_test_until + 1.0) lv_test_stop();
    header(g, "LINK VERSUS", "TWO CONSOLES  ONE PC");
    if (g_lv_mgr) { draw_link_manager(m, g, in); return; }

    move_cursor(PAGE_LINK, kLinkRows, in, kLinkRows);
    int& cur = g_state.cursor[PAGE_LINK];
    if (cur != LV_RESET && cur != LV_ROWS_PER + LV_RESET) g_lv_reset_ask = -1;
    int activate = -1;
    for (int p = 0; p < 2; ++p) {
        const float x = p ? 324.0f : 24.0f;
        panel(g, x, 76.0f, 292.0f, 156.0f, p ? "PLAYER 2" : "PLAYER 1");
        const std::string vals[LV_ROWS_PER] = {
            lv_base(g_lv[p].card), "QUICK START", g_lv_reset_ask == p ? "PRESS A AGAIN" : "", lv_audio_label(p) };
        static const char* const labels[LV_ROWS_PER] = { "SLOT 1", "SLOT 2", "RESET QUICK START", "SOUND" };
        for (int r = 0; r < LV_ROWS_PER; ++r) {
            const int idx = p * LV_ROWS_PER + r;
            bool hovered = false;
            if (row(g, x + 1.0f, 90.0f + (kRowH + 6.0f) * r, 290.0f, cur == idx, true, labels[r],
                    vals[r].c_str(), false, false, &hovered))
                activate = idx;
            if (hovered && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f)) cur = idx;
        }
        /* The sound indicator: a speaker that pulses while this player's
         * output plays its test chime. */
        const float sy = 90.0f + (kRowH + 6.0f) * LV_SOUND;
        const bool playing = g_lv_test_player == p && ImGui::GetTime() < g_lv_test_until;
        const float sx = x + 18.0f, cy = sy + 13.0f;
        const ImU32 col = playing ? kOn : kOff;
        g.dl->AddRectFilled(at(g, sx - 6.0f, cy - 3.0f), at(g, sx - 2.0f, cy + 3.0f), col);
        g.dl->AddTriangleFilled(at(g, sx - 2.0f, cy - 3.0f), at(g, sx + 3.0f, cy - 7.0f), at(g, sx + 3.0f, cy + 7.0f), col);
        g.dl->AddTriangleFilled(at(g, sx - 2.0f, cy - 3.0f), at(g, sx + 3.0f, cy + 7.0f), at(g, sx - 2.0f, cy + 3.0f), col);
        if (playing) {
            const float t = (float)std::fmod(ImGui::GetTime() * 3.0, 1.0);
            for (int w = 0; w < 3; ++w) {
                const float r = 6.0f + 4.0f * w + 3.0f * t;
                g.dl->PathArcTo(at(g, sx + 3.0f, cy), r * g.s, -0.7f, 0.7f, 12);
                g.dl->PathStroke(IM_COL32(95, 224, 138, (int)(220 * (1.0f - t * 0.6f) / (w + 1))), 0, 1.5f * g.s);
            }
            text(g, x + 16.0f, 210.0f, 10.0f, kOn, ("PLAYING ON " + lv_audio_label(p)).c_str(), 270.0f);
        }
    }
    {
        bool hovered = false;
        const int idx = kLinkRows - 1;
        if (row(g, 25.0f, 242.0f, 590.0f, cur == idx, true, "START LINK VERSUS", "", false, false, &hovered))
            activate = idx;
        if (hovered && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f)) cur = idx;
    }

    panel(g, 24.0f, 284.0f, 592.0f, 150.0f, "INFO");
    const int p = cur < kLinkRows - 1 ? cur / LV_ROWS_PER : -1;
    const int r = p >= 0 ? cur % LV_ROWS_PER : -1;
    const char* info =
        r == LV_SLOT1 ? "This player's own memory card. LEFT / RIGHT picks a card from the memory card "
                        "folder or a NEW CARD; A shows its saves (copy or delete)." :
        r == LV_SLOT2 ? "QUICK START: a default AC with every part unlocked, one save without Human "
                        "PLUS and one with it at full. It is this player's own card and keeps what is "
                        "saved to it. A shows its saves." :
        r == LV_RESET ? "Put this player's QUICK START card back to just its two saves. A backup of "
                        "the card is kept." :
        r == LV_SOUND ? "Where this player's sound plays. LEFT / RIGHT picks an output and plays a test "
                        "chime there (one note for player 1, two for player 2); A plays it again." :
                        "Two game sessions on one PC, one per monitor, joined by an emulated link cable. "
                        "Drag each window to its monitor; double-click a window for fullscreen. Hold "
                        "START+SELECT for a second to move a controller to the window clicked last. "
                        "The keyboard always plays for player 1.";
    text(g, 38.0f, 300.0f, 11.0f, kText, info, 560.0f);
    if (!g_lv_status.empty())
        text(g, 38.0f, 410.0f, 11.0f, g_lv_status_bad ? kWarn : kOn, g_lv_status.c_str(), 560.0f);
    footer(g, "A / ENTER  SELECT      LEFT / RIGHT  CHANGE      B / ESC  BACK");

    if (in.back) { set_link_env(false); close_link_page(); return; }
    if (p >= 0 && (in.left || in.right)) {
        const int dir = in.left ? -1 : 1;
        if (r == LV_SLOT1) { lv_step_card(p, dir); g_lv_status.clear(); }
        if (r == LV_SOUND) lv_step_audio(p, dir);
    }
    if (in.confirm) activate = cur;
    if (activate < 0) return;
    cur = activate;
    if (activate == kLinkRows - 1) { start_link_versus(m); return; }
    const int ap = activate / LV_ROWS_PER, ar = activate % LV_ROWS_PER;
    const std::string who = "PLAYER " + std::to_string(ap + 1);
    switch (ar) {
    case LV_SLOT1:
        if (g_lv[ap].card.empty()) { note("NEW CARD: MADE WHEN LINK VERSUS STARTS"); break; }
        lv_open_manager(g_lv[ap].card, who + "  SLOT 1");
        break;
    case LV_SLOT2:
        lv_open_manager(lv_quickstart(m, ap), who + "  SLOT 2  QUICK START");
        break;
    case LV_RESET: {
        if (g_lv_reset_ask != ap) { g_lv_reset_ask = ap; break; }
        g_lv_reset_ask = -1;
        char err[200] = "";
        if (moa_link_quickstart_reset(lv_quickstart(m, ap).c_str(), err, sizeof err))
            note((who + " QUICK START RESET (BACKUP KEPT)").c_str());
        else note(upper(err).c_str());
        break;
    }
    case LV_SOUND:
        lv_test_sound(ap);
        break;
    }
}
