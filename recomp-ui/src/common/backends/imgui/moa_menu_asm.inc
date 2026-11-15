/* Master of Arena system menu: the Assembly screen's extras on top of the AC
 * builder (moa_menu_build.inc includes this).
 *
 *   - the 3D AC, drawn from the optional model install (Extras > INSTALL 3D
 *     AC MODELS; games/master-of-arena/moa_ac_view.c draws it in software);
 *     it turns slowly on its own as in the garage; right stick up / down (or
 *     R / F, or the mouse wheel) zooms
 *   - each part on its own (the garage's part models, same install)
 *   - TRIANGLE / Y (or I): the highlighted part's details, as the game's
 *     garage shows them
 *   - SQUARE / X (or C) in a part list: mark parts of that slot to compare;
 *     green = better than the fitted part, red = worse
 *
 * Part details come from the install's parts/ files; without the install the
 * builder's own six figures are shown.
 */

#include "../../../../../games/master-of-arena/moa_ac_view.h"

/* ---- the model install --------------------------------------------------- */

static std::string models_dir() { return asset("acmodels"); }

static int  g_models_state = 0;          /* 0 not checked, 1 installed, -1 not */
static std::atomic<int> g_models_job{0}; /* 0 idle, 1 running, 2 done ok, 3 failed */
static std::string g_models_status;
static std::mutex g_models_mutex;

static bool models_installed() {
    if (g_models_state == 0) g_models_state = moa_acv_installed(models_dir().c_str()) ? 1 : -1;
    return g_models_state > 0;
}

static void acv_reset();

/* Runs on its own thread: unpack into acmodels.new, keep parts/ motion/
 * models/, then swap it in for any older install. */
static void install_models(const std::string& disc) {
    namespace fs = std::filesystem;
    if (g_models_job.load() == 1) return;
    g_models_job = 1;
    {
        std::lock_guard<std::mutex> lock(g_models_mutex);
        g_models_status = "READING YOUR DISC. THIS TAKES A MINUTE...";
    }
    std::thread([disc]() {
        const fs::path final_dir = fs::u8path(models_dir());
        const fs::path work = fs::u8path(models_dir() + ".new");
        std::error_code ec;
        fs::remove_all(work, ec);
        char err[300] = {0};
        bool ok = moa_acv_import(disc.c_str(), work.u8string().c_str(), err, sizeof err) != 0;
        fs::remove_all(work / "unpacked", ec);   /* the raw archives are not kept */
        if (ok) {
            const fs::path old = fs::u8path(models_dir() + ".old");
            fs::remove_all(old, ec);
            if (fs::exists(final_dir)) fs::rename(final_dir, old, ec);
            ec.clear();
            fs::rename(work, final_dir, ec);
            if (ec) { ok = false; std::snprintf(err, sizeof err, "could not move the models into place"); }
            fs::remove_all(old, ec);
        } else {
            fs::remove_all(work, ec);
        }
        {
            std::lock_guard<std::mutex> lock(g_models_mutex);
            g_models_status = ok ? "INSTALLED. THE BUILDER NOW SHOWS YOUR AC IN 3D."
                                 : upper(err[0] ? err : "THE MODELS COULD NOT BE INSTALLED.");
        }
        g_models_job = ok ? 2 : 3;
    }).detach();
}

static void remove_models() {
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(models_dir()), ec);
    g_models_state = 0;
    acv_reset();
    std::lock_guard<std::mutex> lock(g_models_mutex);
    g_models_status = ec ? "THE MODELS COULD NOT BE REMOVED." : "REMOVED.";
}

static std::string models_status() {
    std::lock_guard<std::mutex> lock(g_models_mutex);
    return g_models_status;
}

/* Called every frame by the menu: pick up a finished install. */
static void models_poll() {
    const int j = g_models_job.load();
    if (j == 2 || j == 3) { g_models_state = 0; acv_reset(); g_models_job = 0; }
}

/* ---- the 3D view ---------------------------------------------------------- */

static moa_acv* g_acv = nullptr;
static uint8_t g_acv_key[13 + 48];
static bool g_acv_have_key = false;
static bool g_acv_failed = false;
static LauncherTexture g_acv_tex = { 0, 0, 0 };
static std::vector<uint8_t> g_acv_px;
static moa_acv* g_part_acv = nullptr;
static std::string g_part_key;
static LauncherTexture g_part_tex = { 0, 0, 0 };
static std::vector<uint8_t> g_part_px;
static float g_acv_yaw = 0.6f, g_acv_pitch = 0.12f;
static float g_acv_zoom = 1.25f;          /* Assembly: about the game's own framing */
static double g_acv_turned = -1.0;       /* frame time the turn was last advanced */

static void acv_reset() {
    moa_acv_free(g_acv);
    g_acv = nullptr;
    moa_acv_free(g_part_acv);
    g_part_acv = nullptr;
    g_part_key.clear();
    g_acv_have_key = false;
    g_acv_failed = false;
}

/* The AC for `rec` and the card's paint, rebuilt when either changes. */
static const moa_acv* acv_for(const uint8_t rec[13], const uint8_t paint[48]) {
    if (!models_installed()) return nullptr;
    uint8_t key[13 + 48];
    std::memcpy(key, rec, 13);
    std::memcpy(key + 13, paint, 48);
    if (!g_acv_have_key || std::memcmp(key, g_acv_key, sizeof key) != 0) {
        moa_acv_free(g_acv);
        g_acv = moa_acv_load(models_dir().c_str(), rec, paint);
        g_acv_failed = g_acv == nullptr;
        std::memcpy(g_acv_key, key, sizeof key);
        g_acv_have_key = true;
    }
    return g_acv;
}

static float stick(ImGuiKey neg, ImGuiKey pos) {
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiKeyData* a = &io.KeysData[neg - ImGuiKey_NamedKey_BEGIN];
    const ImGuiKeyData* b = &io.KeysData[pos - ImGuiKey_NamedKey_BEGIN];
    float v = (b ? b->AnalogValue : 0.0f) - (a ? a->AnalogValue : 0.0f);
    return std::fabs(v) < 0.18f ? 0.0f : v;
}

/* As in the garage the AC (and the part on its own) turns slowly by itself;
 * the only control is zoom: right stick up / down, R / F, or the mouse wheel
 * over the picture. Called once a frame by whatever draws the AC. */
static void acv_input(bool zoomable, ImVec2 a, ImVec2 b) {
    const double now = ImGui::GetTime();
    if (g_acv_turned == now) return;              /* both views share one turn */
    const float dt = ImGui::GetIO().DeltaTime;
    g_acv_turned = now;
    g_acv_yaw += 0.6f * dt;
    if (!zoomable) return;
    float z = stick(ImGuiKey_GamepadRStickDown, ImGuiKey_GamepadRStickUp);
    if (ImGui::IsKeyDown(ImGuiKey_R)) z += 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_F)) z -= 1.0f;
    g_acv_zoom *= std::exp(z * 1.2f * dt);
    if (ImGui::IsMouseHoveringRect(a, b, false) && ImGui::GetIO().MouseWheel != 0.0f)
        g_acv_zoom *= std::pow(1.12f, ImGui::GetIO().MouseWheel);
    g_acv_zoom = std::clamp(g_acv_zoom, 0.7f, 3.0f);
}

/* Draws the AC (or why it can't) in the box at x, y (design units). The
 * part kinds in `wire` (bit per MOA_ACV_* kind) are drawn as `wire_col`
 * wireframe, as the garage marks the slot being changed. */
static void draw_acv(const Gfx& g, float x, float y, float w, float h, const moa_card& card,
                     unsigned wire = 0, ImU32 wire_col = 0, bool zoomable = false) {
    g.dl->AddRectFilled(ImVec2(g.o.x + x * g.s, g.o.y + y * g.s),
                        ImVec2(g.o.x + (x + w) * g.s, g.o.y + (y + h) * g.s), IM_COL32(4, 7, 10, 255));
    if (!models_installed()) {
        text(g, x + 10.0f, y + h * 0.5f - 16.0f, 10.0f, kDim, "NO 3D MODELS INSTALLED", w - 20.0f);
        text(g, x + 10.0f, y + h * 0.5f, 10.0f, kText, "EXTRAS > INSTALL 3D AC MODELS", w - 20.0f);
        return;
    }
    const moa_acv* v = acv_for(card.rec, card.paint);
    if (!v) {
        text(g, x + 10.0f, y + h * 0.5f - 8.0f, 10.0f, kWarn, "NO MODEL FOR THIS CORE / ARMS / LEGS", w - 20.0f);
        return;
    }
    acv_input(zoomable, at(g, x, y), at(g, x + w, y + h));
    /* drawn at the window's own pixel size so it stays sharp */
    const int pw = std::max(16, (int)(w * g.s)), ph = std::max(16, (int)(h * g.s));
    g_acv_px.resize((size_t)pw * ph * 4);
    const uint8_t rgb[3] = { (uint8_t)(wire_col & 255), (uint8_t)((wire_col >> 8) & 255),
                             (uint8_t)((wire_col >> 16) & 255) };
    moa_acv_render_ex(v, g_acv_yaw, g_acv_pitch, (float)ImGui::GetTime(), zoomable ? g_acv_zoom : 1.0f,
                      wire, rgb, pw, ph, g_acv_px.data());
    launcher_texture_set_rgba(&g_acv_tex, g_acv_px.data(), pw, ph);
    if (g_acv_tex.id)
        g.dl->AddImage(tid(g_acv_tex), ImVec2(g.o.x + x * g.s, g.o.y + y * g.s),
                       ImVec2(g.o.x + (x + w) * g.s, g.o.y + (y + h) * g.s));
}

/* ---- one part on its own ---------------------------------------------------- */


/* models/menu folder for a part category (moa_part_cat). */
static const char* part_folder(int cat) {
    switch (cat) {
    case MOA_CAT_HEAD: return "head";
    case MOA_CAT_CORE: return "core";
    case MOA_CAT_LEGS: return "legs";
    case MOA_CAT_ARMS: return "arms";
    case MOA_CAT_HAND: return "hand";
    case MOA_CAT_BACK: return "back";
    case MOA_CAT_BLADE: return "blade";
    case MOA_CAT_BOOST: return "booster";
    case MOA_CAT_FCS: return "fcs";
    case MOA_CAT_GEN: return "generator";
    default: return "";
    }
}

/* The part turning on its own in the box (the garage's left window).
 * Returns false when there is no picture for it (the caller says why). */
static bool draw_kind_view(const Gfx& g, float x, float y, float w, float h, const char* kind, int id,
                           const uint8_t paint[48]) {
    if (!models_installed() || !moa_acv_has_part_models(models_dir().c_str())) return false;
    char key[96];
    std::snprintf(key, sizeof key, "%s/%d/", kind, id);
    std::string k = key;
    k.append((const char*)paint, 48);
    if (k != g_part_key) {
        moa_acv_free(g_part_acv);
        g_part_acv = moa_acv_load_part(models_dir().c_str(), kind, id, paint);
        g_part_key = k;
    }
    if (!g_part_acv) return false;
    acv_input(false, ImVec2(0, 0), ImVec2(0, 0));
    const int pw = std::max(16, (int)(w * g.s)), ph = std::max(16, (int)(h * g.s));
    g_part_px.resize((size_t)pw * ph * 4);
    moa_acv_render_ex(g_part_acv, g_acv_yaw, 0.35f, 0.0f, 0.92f, 0, nullptr, pw, ph, g_part_px.data());
    launcher_texture_set_rgba(&g_part_tex, g_part_px.data(), pw, ph);
    if (g_part_tex.id)
        g.dl->AddImage(tid(g_part_tex), at(g, x, y), at(g, x + w, y + h));
    return true;
}

static bool draw_part_view(const Gfx& g, float x, float y, float w, float h, int cat, int id,
                           const uint8_t paint[48]) {
    return draw_kind_view(g, x, y, w, h, part_folder(cat), id, paint);
}

/* ---- details and comparison ----------------------------------------------- */

/* The part's details: from the install, else the builder's six figures. */
static bool part_details(int cat, int id, moa_acv_details* d) {
    std::memset(d, 0, sizeof *d);
    if (id == 255) return false;
    if (models_installed() && moa_acv_part_details(models_dir().c_str(), cat, id, d)) return true;
    const moa_part* p = moa_parts_find(&g_parts, cat, (uint8_t)id);
    if (!p) return false;
    std::snprintf(d->name, sizeof d->name, "%s", p->name);
    std::snprintf(d->type, sizeof d->type, "%s", moa_part_group(cat, p->name));
    auto add = [&](const char* key, const char* label, long v, int better) {
        if (d->count >= MOA_ACV_FIELDS) return;
        moa_acv_field& f = d->f[d->count++];
        std::snprintf(f.key, sizeof f.key, "%s", key);
        std::snprintf(f.label, sizeof f.label, "%s", label);
        f.value = v;
        f.better = better;
    };
    add("weight", "WEIGHT", p->weight, -1);
    if (cat == MOA_CAT_GEN) add("output", "OUTPUT", p->extra, 1);
    else add("energy_drain", "ENERGY DRAIN", p->drain, -1);
    if (p->ap) add("ap", "AP", p->ap, 1);
    if (p->def_shell || p->def_energy) {
        add("defense_shell", "DEF. SHELL", p->def_shell, 1);
        add("defense_energy", "DEF. ENERGY", p->def_energy, 1);
    }
    if (cat == MOA_CAT_CORE) { add("arm_load_limit", "ARM LOAD", p->extra, 1); add("option_slots", "OPTION SLOTS", p->slots, 1); }
    if (cat == MOA_CAT_LEGS) add("carry_limit", "MAX. WEIGHT", p->extra, 1);
    if (cat == MOA_CAT_BOOST) add("power", "BOOST POWER", p->extra, 1);
    return true;
}

static const moa_acv_field* find_field(const moa_acv_details& d, const char* key) {
    for (int i = 0; i < d.count; ++i) if (std::strcmp(d.f[i].key, key) == 0) return &d.f[i];
    return nullptr;
}

static const ImU32 kBetter = IM_COL32(96, 220, 120, 255);
static const ImU32 kWorse  = IM_COL32(236, 92, 84, 255);

/* Green / red against `ref` for a field (kText when equal or no direction). */
static ImU32 diff_col(const moa_acv_field& f, const moa_acv_details* ref) {
    const moa_acv_field* r = ref ? find_field(*ref, f.key) : nullptr;
    if (!r || f.better == 0 || f.value == r->value) return kText;
    return ((f.value > r->value) == (f.better > 0)) ? kBetter : kWorse;
}

static bool g_details_open = false;
static int  g_details_cat = 0, g_details_id = 255;

static void open_details(int cat, int id) {
    if (id == 255) { note("NO PART FITTED"); return; }
    g_details_open = true;
    g_details_cat = cat;
    g_details_id = id;
}

/* TRIANGLE: one part's page, against the fitted part when it differs. */
static void draw_details(const Gfx& g, int fitted_id) {
    moa_acv_details d, ref;
    const bool have = part_details(g_details_cat, g_details_id, &d);
    const bool cmp = fitted_id != 255 && fitted_id != g_details_id && part_details(g_details_cat, fitted_id, &ref);
    panel(g, 340.0f, 62.0f, 276.0f, 14 * kRowH + 12.0f, "PART DETAILS");
    if (!have) { text(g, 354.0f, 84.0f, 11.0f, kWarn, "NO DETAILS FOR THIS PART"); return; }
    text(g, 354.0f, 78.0f, 13.0f, kBright, d.name);
    text(g, 354.0f, 96.0f, 10.0f, kDim, d.type);
    if (cmp) text_right(g, 594.0f, 96.0f, 9.0f, kDim, "VS FITTED");
    float y = 116.0f;
    char b[48];
    for (int i = 0; i < d.count && y < 420.0f; ++i) {
        text(g, 354.0f, y, 10.0f, kDim, d.f[i].label);
        std::snprintf(b, sizeof b, "%ld", d.f[i].value);
        text_right(g, 594.0f, y, 10.0f, cmp ? diff_col(d.f[i], &ref) : kText, b);
        y += 14.0f;
    }
}

/* SQUARE marks, per record byte (same slot only). */
static std::vector<int> g_marks[13];
static const size_t kMaxMarks = 3;

static void toggle_mark(int byte, int id) {
    if (id == 255) return;
    std::vector<int>& v = g_marks[byte];
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] == id) { v.erase(v.begin() + (long)i); return; }
    if (v.size() >= kMaxMarks) { note("UP TO THREE PARTS CAN BE COMPARED"); return; }
    v.push_back(id);
}

static bool marked(int byte, int id) {
    for (int m : g_marks[byte]) if (m == id) return true;
    return false;
}

/* The fitted part beside each marked one: one column each, every marked
 * value green / red against the fitted part. */
static void draw_compare(const Gfx& g, int byte) {
    const int cat = moa_record_cat(byte);
    const int fitted = g_edit.rec[byte];
    std::vector<int> ids;
    if (fitted != 255) ids.push_back(fitted);
    for (int m : g_marks[byte]) if (m != fitted) ids.push_back(m);
    std::vector<moa_acv_details> d(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) part_details(cat, ids[i], &d[i]);
    const moa_acv_details* ref = fitted != 255 ? &d[0] : nullptr;
    panel(g, 340.0f, 62.0f, 276.0f, 14 * kRowH + 12.0f, "COMPARE");
    /* rows: every field any of them has, in the first part's order */
    std::vector<std::pair<std::string, std::string>> rows;
    for (const moa_acv_details& x : d)
        for (int i = 0; i < x.count; ++i) {
            bool seen = false;
            for (auto& r : rows) if (r.first == x.f[i].key) seen = true;
            if (!seen) rows.push_back({ x.f[i].key, x.f[i].label });
        }
    const float label_w = 96.0f, col_w = (594.0f - 354.0f - label_w) / (float)std::max<size_t>(1, ids.size());
    for (size_t c = 0; c < ids.size(); ++c) {
        const float cx = 354.0f + label_w + col_w * (float)(c + 1);
        std::string n = d[c].name;
        if (n.size() > 9) n.resize(9);
        text_right(g, cx, 78.0f, 9.0f, c == 0 && ref ? kHi : kBright, n.c_str());
        text_right(g, cx, 92.0f, 8.0f, kDim, c == 0 && ref ? "FITTED" : "MARKED");
    }
    float y = 110.0f;
    char b[32];
    for (auto& r : rows) {
        if (y > 420.0f) break;
        std::string lbl = r.second;
        if (lbl.size() > 15) lbl.resize(15);
        text(g, 354.0f, y, 9.0f, kDim, lbl.c_str());
        for (size_t c = 0; c < ids.size(); ++c) {
            const moa_acv_field* f = find_field(d[c], r.first.c_str());
            const float cx = 354.0f + label_w + col_w * (float)(c + 1);
            if (!f) { text_right(g, cx, y, 9.0f, kDim, "-"); continue; }
            std::snprintf(b, sizeof b, "%ld", f->value);
            text_right(g, cx, y, 9.0f, (c == 0 && ref) ? kText : diff_col(*f, ref), b);
        }
        y += 13.0f;
    }
    text(g, 354.0f, 404.0f, 9.0f, kDim, "GREEN BETTER   RED WORSE   THAN FITTED", 240.0f);
}

/* Memory card: the AC as one more save, for VS mode's DATA LOAD (and the
 * guest's card in network play). */
static void save_to_memcard(LauncherModel* m) {
    if (m->in_session) { note("CLOSE THE GAME FIRST"); return; }
    const char* card = m->s.memcard_path[0];
    if (!card || !card[0]) { note("NO MEMORY CARD IS SET"); return; }
    if (!g_edit.ac[0]) std::strcpy(g_edit.ac, "AC");
    char err[160] = {0};
    if (moa_memcard_add_unit(card, &g_edit, err, sizeof err))
        note("SAVED TO THE MEMORY CARD. LOAD IT IN VS MODE: AC SETUP > SYSTEM > DATA LOAD");
    else
        note(upper(err[0] ? err : "THE MEMORY CARD COULD NOT BE WRITTEN").c_str());
}
