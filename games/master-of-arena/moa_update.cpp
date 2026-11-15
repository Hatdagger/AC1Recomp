/* Master of Arena: update check and self-update. See moa_update.h.
 *
 * Network access goes through the system's own curl (SteamOS and Windows 10+
 * both ship one), run without a window; the program carries no TLS code. The
 * release list comes from the GitHub API, the files from the release page, and
 * a download is accepted only when its SHA-256 matches the release's
 * SHA256SUMS.txt.
 *
 * Linux (AppImage): the AppImage file is replaced in place, the old one kept
 * beside it as "<name>.previous". AppRun carries settings, cards and mods to
 * the new build's runtime folder, as it does for any new build.
 * Windows (folder): the release zip is unpacked beside the game and its files
 * are moved over the old ones; the running program is renamed, not deleted,
 * and swept away on the next check. Memory Card, Mods, the player's setting
 * files and disc images are skipped.
 */
#include "moa_update.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

extern "C" {
#include "psx_sha256.h"
}

#ifdef _WIN32
#  include <windows.h>
#else
#  include <spawn.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <fcntl.h>
#  include <unistd.h>
extern char** environ;
#endif

#ifndef MOA_VERSION
#  define MOA_VERSION "dev"
#endif

namespace fs = std::filesystem;

namespace moa_update {
namespace {

const char* const kRepo = "Hatdagger/AC1Recomp";
const char* const kSums = "SHA256SUMS.txt";
#ifdef _WIN32
const char* const kAssetSuffix = "-windows-x64.zip";
#else
const char* const kAssetSuffix = "-linux-x64.AppImage";
#endif

struct Asset { std::string name, url; uint64_t size = 0; };

std::mutex g_mu;
Snapshot   g_snap;
Asset      g_asset, g_sums;       /* what the last check chose */
std::atomic<bool> g_busy{false};

void set(State s, const std::string& msg) {
    std::lock_guard<std::mutex> l(g_mu);
    g_snap.state = s;
    g_snap.message = msg;
}

/* ---- versions ------------------------------------------------------------- */

bool parse_version(const std::string& s, int v[3]) {
    v[0] = v[1] = v[2] = 0;
    size_t i = 0;
    while (i < s.size() && !std::isdigit((unsigned char)s[i])) ++i;
    if (i == s.size()) return false;
    for (int k = 0; k < 3 && i < s.size(); ++k) {
        v[k] = std::atoi(s.c_str() + i);
        while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
        if (i < s.size() && s[i] == '.') ++i; else break;
    }
    return true;
}

int compare(const std::string& a, const std::string& b) {
    int x[3], y[3];
    parse_version(a, x);
    parse_version(b, y);
    for (int i = 0; i < 3; ++i)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

/* ---- running curl --------------------------------------------------------- */

bool safe_url(const std::string& u) {
    static const std::string a = std::string("https://github.com/") + kRepo + "/releases/download/";
    if (u.compare(0, a.size(), a) != 0) return false;
    for (char c : u)
        if (!(std::isalnum((unsigned char)c) || std::strchr(".-_/:%+", c))) return false;
    return true;
}

/* Returns curl's exit code, -1 when curl could not be started. */
int run_curl(const std::vector<std::string>& args) {
#ifdef _WIN32
    wchar_t sys[MAX_PATH] = {0};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring exe = std::wstring(sys) + L"\\curl.exe";
    std::wstring cmd = L"\"" + exe + L"\"";
    for (const std::string& a : args) {
        cmd += L" \"";
        cmd += fs::u8path(a).wstring();
        cmd += L"\"";
    }
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    std::memset(&si, 0, sizeof si); si.cb = sizeof si;
    std::memset(&pi, 0, sizeof pi);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    if (!CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
#else
    std::vector<std::string> av = { "curl" };
    av.insert(av.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& a : av) argv.push_back(a.data());
    argv.push_back(nullptr);

    /* The AppImage points the loader at its own libraries; the system's curl
     * must not see them. */
    const char* appdir = std::getenv("APPDIR");
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) {
        std::string kv(*e);
        if (kv.compare(0, 16, "LD_LIBRARY_PATH=") == 0) {
            std::string kept, rest = kv.substr(16);
            size_t p = 0;
            while (p <= rest.size()) {
                size_t q = rest.find(':', p);
                if (q == std::string::npos) q = rest.size();
                const std::string part = rest.substr(p, q - p);
                if (!part.empty() && !(appdir && appdir[0] && part.compare(0, std::strlen(appdir), appdir) == 0))
                    kept += (kept.empty() ? "" : ":") + part;
                p = q + 1;
            }
            if (kept.empty()) continue;
            kv = "LD_LIBRARY_PATH=" + kept;
        }
        env.push_back(kv);
    }
    std::vector<char*> envp;
    for (std::string& e : env) envp.push_back(e.data());
    envp.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, "curl", &fa, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

std::vector<std::string> curl_args(const std::string& url, const fs::path& out, bool api) {
    std::vector<std::string> a = { "-fsSL", "--connect-timeout", "20", "--max-time", "1800",
                                   "--retry", "2", "-A", "MoA-Updater" };
#ifdef _WIN32
    a.push_back("--ssl-no-revoke");
#endif
    if (api) { a.push_back("-H"); a.push_back("Accept: application/vnd.github+json"); }
    a.push_back("-o"); a.push_back(out.u8string());
    a.push_back(url);
    return a;
}

std::string curl_failure(int rc) {
    if (rc == -1) return "CURL IS NOT INSTALLED ON THIS SYSTEM";
    if (rc == 6 || rc == 7 || rc == 28 || rc == 35 || rc == 56) return "COULD NOT REACH GITHUB. CHECK THE CONNECTION";
    if (rc == 22) return "GITHUB REFUSED THE REQUEST (RATE LIMIT OR NO SUCH FILE)";
    return "THE DOWNLOAD FAILED (CURL ERROR " + std::to_string(rc) + ")";
}

/* ---- the release list ------------------------------------------------------ */

struct Release { std::string tag; std::vector<Asset> assets; };

std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::vector<Release> parse_releases(const std::string& j) {
    std::vector<Release> out;
    const std::string kTag = "\"tag_name\":\"", kUrl = "\"browser_download_url\":\"", kSize = "\"size\":";
    std::vector<size_t> tags;
    for (size_t p = j.find(kTag); p != std::string::npos; p = j.find(kTag, p + 1)) tags.push_back(p);
    for (size_t i = 0; i < tags.size(); ++i) {
        const size_t end = i + 1 < tags.size() ? tags[i + 1] : j.size();
        Release r;
        const size_t t0 = tags[i] + kTag.size();
        r.tag = j.substr(t0, j.find('"', t0) - t0);
        size_t prev = tags[i];
        for (size_t p = j.find(kUrl, tags[i]); p != std::string::npos && p < end; p = j.find(kUrl, p + 1)) {
            Asset a;
            const size_t u0 = p + kUrl.size();
            a.url = j.substr(u0, j.find('"', u0) - u0);
            a.name = a.url.substr(a.url.rfind('/') + 1);
            size_t sp = std::string::npos;
            for (size_t q = j.find(kSize, prev); q != std::string::npos && q < p; q = j.find(kSize, q + 1)) sp = q;
            if (sp != std::string::npos) a.size = std::strtoull(j.c_str() + sp + kSize.size(), nullptr, 10);
            prev = p;
            r.assets.push_back(a);
        }
        out.push_back(r);
    }
    return out;
}

fs::path scratch_dir() {
    std::error_code ec;
    fs::path d = fs::temp_directory_path(ec) / "moa-update";
    fs::create_directories(d, ec);
    return d;
}

/* ---- where this build lives ------------------------------------------------ */

#ifdef _WIN32
fs::path game_root() {      /* the folder that holds "Armored Core Moa.exe" */
    wchar_t buf[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    if (!n || n >= 32768) return {};
    const fs::path exe(buf);
    const fs::path data = exe.parent_path();
    if (data.filename() != "RecompData") return {};
    const fs::path root = data.parent_path();
    std::error_code ec;
    return fs::exists(root / "Armored Core Moa.exe", ec) ? root : fs::path();
}
#else
fs::path appimage_path() {
    const char* p = std::getenv("MOA_APPIMAGE");
    if (!p || !p[0]) return {};
    std::error_code ec;
    return fs::is_regular_file(p, ec) ? fs::path(p) : fs::path();
}
#endif

/* ---- checking a download --------------------------------------------------- */

std::string sha256_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    psx_sha256_ctx c;
    psx_sha256_init(&c);
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), (std::streamsize)buf.size());
        if (f.gcount() > 0) psx_sha256_update(&c, (const uint8_t*)buf.data(), (size_t)f.gcount());
    }
    uint8_t h[32];
    psx_sha256_final(&c, h);
    char hex[65];
    for (int i = 0; i < 32; ++i) std::snprintf(hex + 2 * i, 3, "%02x", h[i]);
    return hex;
}

/* The hash the release lists for a file name; empty when it is not listed. */
std::string listed_hash(const std::string& sums, const std::string& name) {
    size_t p = 0;
    while (p < sums.size()) {
        size_t q = sums.find('\n', p);
        if (q == std::string::npos) q = sums.size();
        std::string line = sums.substr(p, q - p);
        p = q + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t sp = line.find_first_of(" \t");
        if (sp != 64) continue;
        size_t n = sp;
        while (n < line.size() && (line[n] == ' ' || line[n] == '\t' || line[n] == '*')) ++n;
        std::string listed = line.substr(n);          /* "out/Name" when made from a folder */
        listed = listed.substr(listed.find_last_of("/\\") + 1);
        if (listed == name) {
            std::string h = line.substr(0, 64);
            for (char& c : h) c = (char)std::tolower((unsigned char)c);
            return h;
        }
    }
    return "";
}

bool same_content(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::file_size(a, ec) != fs::file_size(b, ec)) return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    std::vector<char> x(1 << 20), y(1 << 20);
    while (fa && fb) {
        fa.read(x.data(), (std::streamsize)x.size());
        fb.read(y.data(), (std::streamsize)y.size());
        if (fa.gcount() != fb.gcount() || std::memcmp(x.data(), y.data(), (size_t)fa.gcount()) != 0) return false;
    }
    return true;
}

#ifdef _WIN32
/* ---- Windows: moving the new files over the old ----------------------------- */

const char* const kOldSuffix = ".moa-old";

void sweep_old(const fs::path& root) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        if (p.extension() == kOldSuffix) { std::error_code r; fs::remove(p, r); }
    }
}

/* Files that belong to the player or to this machine, relative to RecompData
 * (the same list scripts/package_moa_windows.sh keeps). */
bool protected_path(const std::string& rel) {
    static const std::set<std::string> user = {
        "launcher.ini", "launcher-window.ini", "config.ini", "settings.toml", "keybinds.ini",
        "input.ini", "disc.cfg", "bios.cfg", "rom.cfg", "overlay_captures.json",
        "games/master-of-arena/settings.toml", "games/master-of-arena/game_options.toml",
        "games/master-of-arena/keybinds.ini", "games/master-of-arena/input.ini" };
    if (rel.compare(0, 12, "Memory Card/") == 0 || rel.compare(0, 5, "Mods/") == 0 ||
        rel.compare(0, 7, "Update/") == 0)
        return true;
    std::string low = rel;
    for (char& c : low) c = (char)std::tolower((unsigned char)c);
    for (const char* ext : { ".mcd", ".mcr", ".cue", ".bin", ".chd", ".iso", ".img", ".psxmod" })
        if (low.size() > std::strlen(ext) && low.compare(low.size() - std::strlen(ext), std::strlen(ext), ext) == 0)
            return true;
    if (rel.compare(0, 11, "RecompData/") == 0) {
        const std::string sub = rel.substr(11);
        if (user.count(sub)) return true;
        for (const char* d : { "saves/", "mods/", "netplay/", "cache/" })
            if (sub.compare(0, std::strlen(d), d) == 0) return true;
        if (sub.compare(0, 6, "games/") == 0 && sub.find("/disc/") != std::string::npos) return true;
    }
    return false;
}

bool apply_folder(const fs::path& zip, const fs::path& root, std::string& err) {
    std::error_code ec;
    sweep_old(root);
    const fs::path stage = root / "Update" / "stage";
    fs::remove_all(stage, ec);
    fs::create_directories(stage, ec);
    wchar_t sys[MAX_PATH] = {0};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring tar = std::wstring(sys) + L"\\tar.exe";
    std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zip.wstring() + L"\" -C \"" + stage.wstring() + L"\"";
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    std::memset(&si, 0, sizeof si); si.cb = sizeof si; std::memset(&pi, 0, sizeof pi);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    DWORD code = 1;
    if (CreateProcessW(tar.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    const fs::path src = stage / "Armored Core Moa";
    if (code != 0 || !fs::exists(src / "Armored Core Moa.exe", ec) ||
        !fs::exists(src / "RecompData" / "ArmoredCoreMasterOfArena.exe", ec)) {
        fs::remove_all(root / "Update", ec);
        err = "THE UPDATE COULD NOT BE UNPACKED";
        return false;
    }

    struct Move { fs::path dest, old; bool had_old; };
    std::vector<Move> done;
    bool ok = true;
    for (fs::recursive_directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string rel = it->path().lexically_relative(src).generic_u8string();
        if (protected_path(rel)) continue;
        const fs::path dest = root / fs::u8path(rel);
        if (fs::exists(dest, ec) && same_content(it->path(), dest)) continue;
        fs::create_directories(dest.parent_path(), ec);
        Move mv{ dest, fs::path(dest.u8string() + kOldSuffix), false };
        if (fs::exists(dest, ec)) {
            fs::remove(mv.old, ec);
            fs::rename(dest, mv.old, ec);
            if (ec) { ok = false; break; }
            mv.had_old = true;
        }
        fs::rename(it->path(), dest, ec);
        if (ec) {
            if (mv.had_old) fs::rename(mv.old, dest, ec);
            ok = false;
            break;
        }
        done.push_back(mv);
    }
    if (!ok) {   /* put every file back as it was */
        for (auto i = done.rbegin(); i != done.rend(); ++i) {
            fs::remove(i->dest, ec);
            if (i->had_old) fs::rename(i->old, i->dest, ec);
        }
        fs::remove_all(root / "Update", ec);
        err = "A FILE WAS IN USE. NOTHING WAS CHANGED";
        return false;
    }
    fs::remove_all(root / "Update", ec);
    return true;
}
#endif

/* ---- the two jobs ------------------------------------------------------------ */

void do_check() {
    const fs::path out = scratch_dir() / "releases.json";
    std::error_code ec;
    fs::remove(out, ec);
    const std::string api = std::string("https://api.github.com/repos/") + kRepo + "/releases?per_page=15";
    const int rc = run_curl(curl_args(api, out, true));
    if (rc != 0) { set(State::Failed, curl_failure(rc)); return; }
    const std::vector<Release> rels = parse_releases(read_all(out));
    fs::remove(out, ec);

    const Release* best = nullptr;
    Asset pick, sums;
    for (const Release& r : rels) {
        if (r.tag.compare(0, 5, "moa-v") != 0) continue;
        Asset a, s;
        for (const Asset& x : r.assets) {
            if (x.name.size() > std::strlen(kAssetSuffix) &&
                x.name.compare(x.name.size() - std::strlen(kAssetSuffix), std::string::npos, kAssetSuffix) == 0) a = x;
            if (x.name == kSums) s = x;
        }
        if (a.url.empty() || s.url.empty()) continue;
        if (!best || compare(r.tag, best->tag) > 0) { best = &r; pick = a; sums = s; }
    }
    if (!best) { set(State::Failed, "NO RELEASE FOR THIS SYSTEM WAS FOUND"); return; }
    std::string latest = best->tag.substr(5);
    if (!latest.empty() && latest[0] == 'v') latest.erase(0, 1);
    std::lock_guard<std::mutex> l(g_mu);
    g_snap.latest = latest;
    g_asset = pick; g_sums = sums;
    g_snap.total = pick.size;
    const std::string cur = MOA_VERSION;
    if (cur == "dev" || compare(latest, cur) > 0) {
        g_snap.state = State::Available;
        g_snap.message = "VERSION " + latest + " IS AVAILABLE";
    } else {
        g_snap.state = State::UpToDate;
        g_snap.message = "YOU HAVE THE LATEST VERSION";
    }
}

void do_apply() {
    std::string why;
    if (!supported(&why)) { set(State::Failed, why); return; }
    Asset asset, sums;
    std::string latest;
    { std::lock_guard<std::mutex> l(g_mu); asset = g_asset; sums = g_sums; latest = g_snap.latest; }
    if (!safe_url(asset.url) || !safe_url(sums.url)) { set(State::Failed, "THE RELEASE POINTS SOMEWHERE UNEXPECTED"); return; }

    std::error_code ec;
#ifdef _WIN32
    const fs::path root = game_root();
    const fs::path dir = root / "Update";
    const fs::path file = dir / asset.name;
#else
    const fs::path target = appimage_path();
    const fs::path dir = scratch_dir();
    fs::path file = target; file += ".update";      /* same folder: the swap is one rename */
#endif
    fs::create_directories(dir, ec);
    { std::lock_guard<std::mutex> l(g_mu); g_snap.state = State::Downloading; g_snap.message = "DOWNLOADING"; g_snap.done = 0; g_snap.total = asset.size; }

    const fs::path sumfile = dir / kSums;
    fs::remove(sumfile, ec);
    int rc = run_curl(curl_args(sums.url, sumfile, false));
    if (rc != 0) { set(State::Failed, curl_failure(rc)); return; }
    const std::string want = listed_hash(read_all(sumfile), asset.name);
    fs::remove(sumfile, ec);
    if (want.empty()) { set(State::Failed, "THE RELEASE HAS NO CHECKSUM FOR THIS FILE"); return; }

    fs::remove(file, ec);
    std::atomic<bool> finished{false};
    std::thread meter([&] {
        while (!finished) {
            std::error_code e;
            const uint64_t n = fs::exists(file, e) ? (uint64_t)fs::file_size(file, e) : 0;
            { std::lock_guard<std::mutex> l(g_mu); if (g_snap.state == State::Downloading) g_snap.done = n; }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });
    rc = run_curl(curl_args(asset.url, file, false));
    finished = true;
    meter.join();
    if (rc != 0) { fs::remove(file, ec); set(State::Failed, curl_failure(rc)); return; }

    set(State::Applying, "CHECKING THE DOWNLOAD");
    if (sha256_file(file) != want) {
        fs::remove(file, ec);
        set(State::Failed, "THE DOWNLOAD DID NOT MATCH ITS CHECKSUM. NOTHING WAS CHANGED");
        return;
    }
    set(State::Applying, "INSTALLING");
    std::string err;
#ifdef _WIN32
    const bool ok = apply_folder(file, root, err);
    if (ok) {
        std::error_code r;
        fs::remove_all(dir, r);
        set(State::Done, "UPDATED TO " + latest + ". CLOSE THE GAME AND START IT AGAIN");
    } else {
        std::error_code r;
        fs::remove_all(dir, r);
        set(State::Failed, err);
    }
#else
    chmod(file.c_str(), 0755);
    fs::path prev = target; prev += ".previous";
    fs::remove(prev, ec);
    fs::rename(target, prev, ec);
    if (ec) { fs::remove(file, ec); set(State::Failed, "THE APPIMAGE COULD NOT BE REPLACED (IS THE FOLDER WRITABLE?)"); return; }
    fs::rename(file, target, ec);
    if (ec) {
        std::error_code r;
        fs::rename(prev, target, r);
        fs::remove(file, r);
        set(State::Failed, "THE APPIMAGE COULD NOT BE REPLACED. NOTHING WAS CHANGED");
        return;
    }
    set(State::Done, "UPDATED TO " + latest + ". CLOSE THE GAME AND START IT AGAIN. THE OLD VERSION IS KEPT AS " +
                     [&] { std::string n = prev.filename().u8string(); for (char& c : n) c = (char)std::toupper((unsigned char)c); return n; }());
#endif
}

void start(void (*job)()) {
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) return;
    std::thread([job] { job(); g_busy = false; }).detach();
}

} /* namespace */

const char* current_version() { return MOA_VERSION; }

bool supported(std::string* why) {
    const char* reason = nullptr;
#ifdef _WIN32
    if (game_root().empty()) reason = "UPDATING NEEDS THE \"ARMORED CORE MOA\" FOLDER LAYOUT";
#elif defined(__linux__)
    if (appimage_path().empty()) reason = "UPDATING NEEDS THE APPIMAGE BUILD";
#else
    reason = "UPDATING IS NOT AVAILABLE ON THIS SYSTEM";
#endif
    if (why) *why = reason ? reason : "";
    return reason == nullptr;
}

Snapshot snapshot() {
    std::lock_guard<std::mutex> l(g_mu);
    Snapshot s = g_snap;
    s.current = MOA_VERSION;
    return s;
}

void check() {
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (g_busy) return;
        g_snap.state = State::Checking;
        g_snap.message = "LOOKING FOR UPDATES";
    }
    start(do_check);
}

void download_and_apply() {
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (g_busy || g_snap.state != State::Available) return;
        g_snap.state = State::Downloading;      /* at once, so a second press finds it busy */
        g_snap.message = "DOWNLOADING";
        g_snap.done = 0;
    }
    start(do_apply);
}

} /* namespace moa_update */
