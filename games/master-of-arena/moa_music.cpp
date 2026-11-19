/* Rogue Arena music: the game's menu song managed around the run's screens,
 * and the player's own music files by scene and belt (moa_music.h).
 *
 * The game's menu song (menu module FDAT 206, read 2026-11-18):
 * - 0x80064F24(a0 = 2) makes a task (0x80064FAC) that loads the music bank
 *   (0x80058700), waits for the loads (flags 0x801D0B21..23) and the VAB
 *   transfer (0x80065060), then 0x8006509C opens the sequence (SsSeqOpen
 *   0x8001D364), sets its volume (SsSeqSetVol 0x800216B4, the options' BGM
 *   byte 0x8004B59A x 2), plays it (SsSeqPlay 0x8001F7D4, endless) and sets
 *   0x801D0AFC = 1. The song's access number is the s16 at 0x801D0B04 + 2 x
 *   slot (slot = byte 0x801D0B20). Called by the hub open (0x80056DBC, from
 *   0x800552C0) and by the two arena returns (0x800ACF58, 0x800B1598).
 * - The menu's entry (0x800A69C8) clears 0x801D0AFC; nothing else does.
 * - SsSeqSetVol on a playing song changes the sounding voices at once.
 *
 * Custom files: <music folder>/<name>.ogg|.mp3|.wav, decoded on a worker
 * thread, played on the runtime's host music channel. */
#include "moa_music.h"
#include "cpu_state.h"
#include "mod_plugins.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#include "third_party/stb_vorbis.c"
#include "third_party/dr_mp3.h"
const char *moa_hub_rank_belt(int rank);
}

namespace fs = std::filesystem;

/* ---- the game's song ------------------------------------------------------- */
#define MUS_PLAYING     0x801D0AFCu   /* 1 once SsSeqPlay ran (cleared at menu entry) */
#define MUS_LOADING     0x801D0B21u   /* 4 bytes: a start in flight while any is set */
#define MUS_SEQ         0x801D0B04u   /* s16 access number per slot */
#define MUS_SLOT        0x801D0B20u
#define MUS_START       0x80064F24u   /* (2) */
#define MUS_SIG_START   0x27BDFFE8u
#define MUS_SET_VOL     0x800216B4u   /* exe SsSeqSetVol(seq, l, r) */
#define MUS_SIG_SET_VOL 0x27BDFFE8u
#define MUS_USER_VOL    0x8004B59Au   /* options: BGM volume (x 2 for the song) */

static moa_music_log_fn s_log;
#define LOG(...) do { if (s_log) s_log(__VA_ARGS__); } while (0)

static int s_scene = MOA_MUSIC_OFF, s_rank = 50;
static int s_own_start;              /* this menu session's song was started by the run */
static int s_start_due;              /* start the song at the next menu frame */
static int s_start_tries;            /* run starts this menu session */
static uint32_t s_silent;            /* hub frames with no song and none loading */
static uint32_t s_loading;           /* frames a start has been in flight */
static int s_told_stuck;
static float s_level = 1.0f;         /* the game's song, 0..1 of the player's BGM volume */
static float s_want = 1.0f;
static int s_dirty;                  /* the run changed the song's volume: put it back at 1 */
static int s_set = -1, s_set_seq = -1, s_was_playing;

static int song_playing(void) { return psx_mod_read_byte(MUS_PLAYING) == 1u; }
static int song_loading(void)
{
    for (uint32_t i = 0; i < 4u; i++) if (psx_mod_read_byte(MUS_LOADING + i)) return 1;
    return 0;
}

/* ---- custom files ------------------------------------------------------------ */
struct Track {
    std::vector<int16_t> pcm;          /* stereo */
    uint32_t rate = 0, loop = 0;
};
static std::mutex s_mx;
static std::atomic<uint32_t> s_gen{0};
static Track *s_ready;                  /* decoded, waiting for the emulation thread */
static uint32_t s_ready_gen;
static std::string s_ready_err;
static std::string s_playing;           /* the file on the channel ("" none) */
static std::string s_wanted;            /* the file asked for ("" none) */
static int s_custom_on;
static int s_pending;                   /* a file is being decoded */
static std::string s_dir;
static int s_dir_done;

static std::string lower(std::string s)
{
    for (auto &c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

static const char k_readme[] =
"Rogue Arena music\n"
"=================\n"
"\n"
"Put your own music here and Rogue Arena plays it instead of the game's menu song.\n"
"Formats: .ogg, .mp3 or .wav. Any length; it loops.\n"
"\n"
"File names (first one found is played):\n"
"  boot          while the run loads, before you have control\n"
"  hub_<belt>    the hub and its menus at that belt, e.g. hub_white, hub_black-gold\n"
"  hub           the hub and its menus at any belt (and the boot, if there is no boot file)\n"
"  victory       the victory screen (instead of silence after the victory jingle)\n"
"  loss          the RUN OVER screen (otherwise the hub's music keeps playing)\n"
"\n"
"Belts: white, yellow, orange, green, blue, purple, red, brown, black, black-red, black-gold\n"
"(rank 50-46 white ... 10-6 black, 5-2 black-red, 1 black-gold).\n"
"\n"
"The music changes by itself as the screens and your belt change (a short crossfade).\n"
"Fights keep the game's own fight music.\n"
"\n"
"Loop point (optional, .ogg only): a LOOPSTART=<sample> comment tag, as RPG Maker uses;\n"
"LOOPLENGTH=<samples> ends the loop early. Without it the whole file loops.\n"
"\n"
"No files here: the game's own menu song plays.\n";

static void find_dir(void)
{
    if (s_dir_done) return;
    s_dir_done = 1;
    std::error_code ec;
    fs::path d;
    const char *e = getenv("PSX_MOA_MUSIC_DIR");
    if (e && e[0]) d = fs::u8path(e);
    else {
        const char *m = getenv("PSX_MODS_DIR");             /* <release>/Mods */
        if (!m || !m[0]) return;
        d = fs::u8path(m).parent_path() / "Music" / "Rogue Arena";
    }
    fs::create_directories(d, ec);
    if (!fs::is_directory(d, ec)) { LOG("music: no music folder (%s)", d.u8string().c_str()); return; }
    s_dir = d.u8string();
    const fs::path readme = d / "README.txt";
    if (!fs::exists(readme, ec)) {
        FILE *f = fopen(readme.u8string().c_str(), "wb");
        if (f) { fputs(k_readme, f); fclose(f); }
    }
    LOG("music: custom music folder %s", s_dir.c_str());
}

/* The first file for these names, "" none. Case of the name and extension
 * does not matter. */
static std::string pick(const std::vector<std::string> &names)
{
    if (s_dir.empty()) return "";
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(fs::u8path(s_dir), ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) files.push_back(it->path());
    static const char *const k_ext[] = { ".ogg", ".mp3", ".wav" };
    for (const auto &n : names)
        for (const char *x : k_ext)
            for (const auto &p : files)
                if (lower(p.filename().u8string()) == lower(n) + x) return p.u8string();
    return "";
}

static std::vector<std::string> names_for(int scene, int rank)
{
    const std::string belt = std::string("hub_") + moa_hub_rank_belt(rank);
    switch (scene) {
    case MOA_MUSIC_BOOT:    return { "boot", belt, "hub" };
    case MOA_MUSIC_HUB:     return { belt, "hub" };
    case MOA_MUSIC_VICTORY: return { "victory" };
    case MOA_MUSIC_LOSS:    return { "loss", belt, "hub" };
    default:                return {};
    }
}

/* ---- decoding (worker thread) ------------------------------------------------ */
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static bool decode_wav(const std::string &path, Track &t, std::string &err)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open"; return false; }
    std::vector<uint8_t> b;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0) { b.resize((size_t)n); if (fread(b.data(), 1, b.size(), f) != b.size()) b.clear(); }
    fclose(f);
    if (b.size() < 12 || memcmp(b.data(), "RIFF", 4) || memcmp(b.data() + 8, "WAVE", 4)) { err = "not a WAV file"; return false; }
    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *data = nullptr;
    size_t bytes = 0;
    for (size_t at = 12; at + 8 <= b.size();) {
        const uint32_t sz = rd32(&b[at + 4]);
        const uint8_t *c = &b[at + 8];
        const size_t have = b.size() - (at + 8);
        if (!memcmp(&b[at], "fmt ", 4) && sz >= 16 && have >= 16) {
            fmt = rd16(c); ch = rd16(c + 2); rate = rd32(c + 4); bits = rd16(c + 14);
            if (fmt == 0xFFFE && sz >= 26) fmt = rd16(c + 24);       /* extensible: the sub-format */
        } else if (!memcmp(&b[at], "data", 4)) {
            data = c;
            bytes = sz < have ? sz : have;
        }
        at += 8 + (size_t)sz + (sz & 1u);
    }
    if (!data || !ch || !rate) { err = "no audio in the WAV file"; return false; }
    const bool flt = fmt == 3 && bits == 32;
    if (!(fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) && !flt) { err = "unsupported WAV encoding"; return false; }
    const size_t bps = bits / 8u, frames = bytes / (bps * ch);
    t.rate = rate;
    t.pcm.resize(frames * 2u);
    for (size_t i = 0; i < frames; i++)
        for (unsigned k = 0; k < 2u; k++) {
            const uint8_t *s = data + (i * ch + (k < ch ? k : 0u)) * bps;
            int v;
            if (flt) { float x; memcpy(&x, s, 4); v = (int)(x * 32767.0f); }
            else if (bits == 8) v = ((int)s[0] - 128) << 8;
            else if (bits == 16) v = (int16_t)rd16(s);
            else if (bits == 24) v = (int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24) >> 16;
            else v = (int32_t)rd32(s) >> 16;
            t.pcm[i * 2u + k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    return true;
}

static bool decode_ogg(const std::string &path, Track &t, std::string &err)
{
    int e = 0;
    stb_vorbis *v = stb_vorbis_open_filename(path.c_str(), &e, nullptr);
    if (!v) { err = "not an Ogg Vorbis file"; return false; }
    const stb_vorbis_info info = stb_vorbis_get_info(v);
    uint64_t loop_start = 0, loop_len = 0;
    const stb_vorbis_comment cm = stb_vorbis_get_comment(v);
    for (int i = 0; i < cm.comment_list_length; i++) {
        const std::string c = lower(cm.comment_list[i]);
        if (c.rfind("loopstart=", 0) == 0) loop_start = strtoull(c.c_str() + 10, nullptr, 10);
        else if (c.rfind("looplength=", 0) == 0) loop_len = strtoull(c.c_str() + 11, nullptr, 10);
    }
    t.rate = info.sample_rate;
    short buf[4096 * 2];
    for (;;) {
        const int n = stb_vorbis_get_samples_short_interleaved(v, 2, buf, 4096 * 2);
        if (n <= 0) break;
        t.pcm.insert(t.pcm.end(), buf, buf + (size_t)n * 2u);
    }
    stb_vorbis_close(v);                       /* (mono comes out on both sides) */
    const uint64_t frames = t.pcm.size() / 2u;
    if (loop_len && loop_start + loop_len < frames) t.pcm.resize((size_t)(loop_start + loop_len) * 2u);
    t.loop = loop_start < t.pcm.size() / 2u ? (uint32_t)loop_start : 0u;
    if (t.pcm.empty()) { err = "no audio in the Ogg file"; return false; }
    return true;
}

static bool decode_mp3(const std::string &path, Track &t, std::string &err)
{
    drmp3_config cfg;
    drmp3_uint64 frames = 0;
    drmp3_int16 *p = drmp3_open_file_and_read_pcm_frames_s16(path.c_str(), &cfg, &frames, nullptr);
    if (!p || !frames || !cfg.channels) { if (p) drmp3_free(p, nullptr); err = "not an MP3 file"; return false; }
    t.rate = cfg.sampleRate;
    t.pcm.resize((size_t)frames * 2u);
    for (size_t i = 0; i < frames; i++) {
        t.pcm[i * 2u] = p[i * cfg.channels];
        t.pcm[i * 2u + 1u] = p[i * cfg.channels + (cfg.channels > 1 ? 1u : 0u)];
    }
    drmp3_free(p, nullptr);
    return true;
}

static void decode_job(std::string path, uint32_t gen)
{
    Track *t = new Track;
    std::string err;
    const std::string x = lower(fs::u8path(path).extension().u8string());
    const bool ok = x == ".ogg" ? decode_ogg(path, *t, err) : x == ".mp3" ? decode_mp3(path, *t, err)
                                                            : decode_wav(path, *t, err);
    if (ok && (t->rate < 8000u || t->rate > 192000u)) { err = "unsupported sample rate"; }
    std::lock_guard<std::mutex> g(s_mx);
    if (gen != s_gen.load()) { delete t; return; }       /* asked for something else since */
    delete s_ready;
    s_ready = nullptr;
    s_ready_gen = gen;
    s_ready_err = (ok && err.empty()) ? "" : (err.empty() ? "could not be read" : err);
    if (s_ready_err.empty()) s_ready = t;
    else delete t;
}

/* Ask for `path` ("" = none: back to the game's song). */
static void want_file(const std::string &path)
{
    if (path == s_wanted) return;
    s_wanted = path;
    const uint32_t gen = ++s_gen;
    if (path.empty()) {
        if (!s_playing.empty()) { psx_mod_music_stop(1200); LOG("music: custom music off, the game's song"); }
        s_playing.clear();
        s_custom_on = 0;
        s_pending = 0;
        return;
    }
    s_pending = path != s_playing;
    if (!s_pending) return;
    std::thread(decode_job, path, gen).detach();
}

/* A decoded file arrived: on the channel with a crossfade. */
static void poll_ready(void)
{
    Track *t = nullptr;
    std::string err;
    uint32_t gen = 0;
    {
        std::lock_guard<std::mutex> g(s_mx);
        if (!s_ready && s_ready_err.empty()) return;
        t = s_ready; s_ready = nullptr;
        err = s_ready_err; s_ready_err.clear();
        gen = s_ready_gen;
    }
    if (gen != s_gen.load()) { delete t; return; }
    s_pending = 0;
    const std::string name = fs::u8path(s_wanted).filename().u8string();
    if (!t) {
        LOG("music: %s: %s; the game's song plays", name.c_str(), err.c_str());
        if (!s_playing.empty()) psx_mod_music_stop(1200);
        s_playing.clear();
        s_custom_on = 0;
        return;
    }
    const uint32_t frames = (uint32_t)(t->pcm.size() / 2u);
    if (psx_mod_music_play(t->pcm.data(), frames, t->rate, t->loop, 1500)) {
        s_playing = s_wanted;
        s_custom_on = 1;
        LOG("music: playing %s (%u s, %u Hz, loop from %u)", name.c_str(),
            (unsigned)(frames / t->rate), (unsigned)t->rate, (unsigned)t->loop);
        psx_mod_counter_add("moa.music.custom", 1);
    } else LOG("music: %s could not be played (memory)", name.c_str());
    delete t;
}

/* ---- per frame -------------------------------------------------------------- */
extern "C" void moa_music_reset(moa_music_log_fn log)
{
    s_log = log;
    psx_mod_music_stop(0);
    psx_mod_music_level(1000, 0);
    ++s_gen;
    {
        std::lock_guard<std::mutex> g(s_mx);
        delete s_ready; s_ready = nullptr; s_ready_err.clear();
    }
    s_playing.clear(); s_wanted.clear(); s_custom_on = 0; s_pending = 0;
    s_scene = MOA_MUSIC_OFF; s_rank = 50;
    s_own_start = s_start_due = s_start_tries = 0;
    s_silent = s_loading = 0; s_told_stuck = 0;
    s_level = s_want = 1.0f; s_dirty = 0; s_set = s_set_seq = -1; s_was_playing = 0;
}

extern "C" int moa_music_custom_on(void) { return s_custom_on; }

extern "C" void moa_music_frame(int scene, int rank)
{
    if (scene == MOA_MUSIC_OFF && s_scene == MOA_MUSIC_OFF) return;
    if (scene != MOA_MUSIC_OFF) find_dir();
    if (scene != s_scene || rank != s_rank) {
        if (scene != s_scene) LOG("music: scene %d -> %d (rank %d)", s_scene, scene, rank);
        s_scene = scene;
        s_rank = rank;
        want_file(scene == MOA_MUSIC_FIGHT || scene == MOA_MUSIC_OFF ? std::string() : pick(names_for(scene, rank)));
    }
    poll_ready();
    /* The game's song: silent under a custom file and the victory screen;
     * back over 2 s (down over 0.5 s). */
    s_want = (s_custom_on || s_pending || scene == MOA_MUSIC_VICTORY) ? 0.0f : 1.0f;
    if (s_level < s_want) { s_level += 1.0f / 120.0f; if (s_level > s_want) s_level = s_want; }
    else if (s_level > s_want) { s_level -= 1.0f / 30.0f; if (s_level < s_want) s_level = s_want; }
    /* No song at the hub: start it (twice at most per menu session). */
    if (scene == MOA_MUSIC_HUB && psx_mod_read_word(MUS_START) == MUS_SIG_START) {
        if (song_playing()) { s_silent = s_loading = 0; }
        else if (song_loading()) {
            s_silent = 0;
            if (++s_loading == 900u && !s_told_stuck) {
                s_told_stuck = 1;
                LOG("music: the menu song has been loading for 15 s (stuck?)");
            }
        } else if (++s_silent == 120u && s_start_tries < 2) {
            s_start_due = 1;
            LOG("music: no menu song at the hub after 2 s, starting it");
        }
    } else s_silent = 0;
}

extern "C" void moa_music_menu_entry(void)
{
    s_own_start = s_start_due = s_start_tries = 0;
    s_silent = s_loading = 0;
    s_set = s_set_seq = -1;
    s_was_playing = 0;
}

static void song_start(struct CPUState *cpu, uint32_t ra, const char *why)
{
    s_own_start = 1;
    s_start_tries++;
    (void)psx_mod_call_guest(cpu, MUS_START, ra, 2u, 0u, 0u, 0u);
    psx_mod_counter_add("moa.music.song_started", 1);
    LOG("music: menu song started (%s)", why);
}

extern "C" void moa_music_menu_frame(struct CPUState *cpu)
{
    if (s_scene == MOA_MUSIC_OFF) return;
    const uint32_t sp0 = cpu->gpr[29];
    if (s_start_due) {
        s_start_due = 0;
        if (psx_mod_read_word(MUS_START) == MUS_SIG_START && !song_playing() && !song_loading()) {
            cpu->gpr[29] = sp0 - 64u;
            song_start(cpu, cpu->gpr[31], "the hub had none");
            cpu->gpr[29] = sp0;
        }
    }
    const int playing = song_playing();
    if (!playing) { s_was_playing = 0; return; }
    const int slot = psx_mod_read_byte(MUS_SLOT) & 1;
    const int seq = (int16_t)psx_mod_read_half(MUS_SEQ + 2u * (uint32_t)slot);
    if (seq < 0 || seq > 31) return;
    if (!s_was_playing || seq != s_set_seq) { s_set = -1; s_was_playing = 1; }   /* (re)started: game volume */
    if (s_level >= 1.0f && !s_dirty) return;                  /* never touched: the game's own volume */
    int user = psx_mod_read_byte(MUS_USER_VOL);
    if (user > 63) user = 63;
    const int v = (int)(user * 2 * s_level + 0.5f);
    if (v == s_set && seq == s_set_seq) return;
    if (psx_mod_read_word(MUS_SET_VOL) != MUS_SIG_SET_VOL) return;
    cpu->gpr[29] = sp0 - 64u;
    (void)psx_mod_call_guest(cpu, MUS_SET_VOL, cpu->gpr[31], (uint32_t)seq, (uint32_t)v, (uint32_t)v, 0u);
    cpu->gpr[29] = sp0;
    s_set = v;
    s_set_seq = seq;
    s_dirty = s_level < 1.0f;
}

extern "C" int moa_music_start_filter(struct CPUState *cpu, uint32_t address)
{
    if (s_scene == MOA_MUSIC_OFF || psx_mod_read_word(address) != MUS_SIG_START) return 0;
    if (cpu->gpr[4] != 2u || !s_own_start || !(song_playing() || song_loading())) return 0;
    cpu->gpr[2] = 0;
    LOG("music: the hub's own song start skipped (the run's is %s)", song_playing() ? "playing" : "loading");
    return 1;
}

extern "C" int moa_music_boot_start(struct CPUState *cpu, uint32_t ra)
{
    if (psx_mod_read_word(MUS_START) != MUS_SIG_START) { LOG("music: song start code not in memory"); return 0; }
    const char *e = getenv("PSX_MOA_RUN_EARLY_MUSIC");
    if (e && e[0] == '0') return 0;
    if (song_playing() || song_loading()) return 0;
    song_start(cpu, ra, "boot");
    return 1;
}
