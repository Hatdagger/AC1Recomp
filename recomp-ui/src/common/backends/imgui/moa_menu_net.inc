/* Master of Arena system menu: the network pages.
 *
 * Included by moa_menu.inc, inside namespace moa_menu. It owns the launcher's
 * three netplay views:
 *
 *   LNG_VIEW_NETPLAY_MODE   LAN or online, and the callsign
 *   LNG_VIEW_NETPLAY        the room list: host, join, join by address
 *   LNG_VIEW_LOBBY          the versus room: pilots, garage, match settings,
 *                           READY (the match starts when both are ready)
 *
 * Every page works from the D-pad and two buttons. Nothing here asks for a
 * keyboard: the callsign is picked from the arena roster, a room is hosted
 * with one press, and an address is dialled a field at a time. The stock
 * dialogs (room name and password, typed address, match settings, room mods)
 * are still one row away for anyone with a keyboard, and FULL NETWORK PAGE
 * hands the whole thing back to the stock pages.
 *
 * The lobby backend is driven through the same np_* helpers the stock pages
 * use, in the same order each frame, so the two are interchangeable.
 *
 * Expected from the including file, beyond what moa_menu.inc lists: np_cb()
 * and the np_* helpers, LobbySnapshot, launcher_udp_port.h, and
 * moa_menu_lobby_popups().
 */

/* Arena pilots. A new player is given one of these as a callsign. */
static const char* const kCallsigns[] = {
    "Pandora", "Max", "Rob", "Carson", "Pascal", "Fayde", "Spin Kid",
    "Sherbert", "Prime", "William", "Sweet Devil", "Fritz", "Bugzy", "Inky",
    "Armadillo", "Flash", "Maeve", "Frog Man", "Ideal", "Noble", "Advantage",
    "Stunt Rider", "Bibs", "Blaster", "Triad", "Ivy", "Decorator", "Blitz",
    "Top Cap", "Evolve", "Welder", "Fletch", "Bomber", "Flipper", "Bolt",
    "Target", "Gryphon", "Libra", "Ernst", "Moon", "Vlad", "Rianon",
    "Adverse", "Seafarer", "Zweit", "Tall Spot", "Searcher", "Karl",
    "Lynx Minx", "Solitude", "Orbit", "Dynamite", "Kroeger", "Trust",
    "Bloom", "Swimmer", "Tread", "Milo", "Bullet", "Scorch", "Fenrir",
    "Moby", "Warren", "Artemis", "Darkhawk", "Siren", "Napoleon", "Fokker",
    "Masquerade", "Feint", "Clover", "Adler", "Freeway", "Gabrielle",
    "Firecracker", "Phantom", "Detective",
};
static const int kCallsignCount = (int)(sizeof kCallsigns / sizeof kCallsigns[0]);

static void set_callsign(LauncherModel* m, const char* name) {
    /* What the stock name dialog does on Save. */
    char previous_default[96];
    std::snprintf(previous_default, sizeof previous_default, "%s's Lobby",
                  m->s.netplay_player_name);
    const bool rename_room = !m->netplay_host_name[0] ||
        std::strcmp(m->netplay_host_name, previous_default) == 0;
    std::snprintf(m->s.netplay_player_name, sizeof m->s.netplay_player_name,
                  "%s", name);
    if (rename_room)
        std::snprintf(m->netplay_host_name, sizeof m->netplay_host_name,
                      "%s's Lobby", np_effective_name(m));
    const auto* np = np_cb(m);
    if (np && np->set_player_name)
        np->set_player_name(np->ctx, m->s.netplay_player_name);
}

static int callsign_index(const LauncherModel* m) {
    for (int i = 0; i < kCallsignCount; ++i)
        if (std::strcmp(kCallsigns[i], m->s.netplay_player_name) == 0) return i;
    return -1;
}

static void step_callsign(LauncherModel* m, int dir) {
    int i = callsign_index(m);
    if (i < 0) i = dir > 0 ? -1 : 0;
    i = (i + (dir > 0 ? 1 : kCallsignCount - 1)) % kCallsignCount;
    set_callsign(m, kCallsigns[i]);
}

/* Once per run, and a callsign for anyone who has none: the stock page asks
 * for a typed name before it shows anything. */
static void net_setup(LauncherModel* m) {
    if (!g_state.net_ready) {
        g_state.net_ready = true;
        np_load_network_settings(m);
        np_pin_host_ice_relay(m);
        np_refresh_host_ip(m);
    }
    if (!np_effective_name(m)[0]) {
        const unsigned pick = (unsigned)(ImGui::GetTime() * 100000.0) +
                              (unsigned)ImGui::GetFrameCount() * 7919u;
        set_callsign(m, kCallsigns[pick % (unsigned)kCallsignCount]);
    }
    m->netplay_name_prompted = true;
}

/* What every netplay page owes the backend each frame. */
static void net_tick(LauncherModel* m, const RecompLauncherCNetplayCallbacks* np) {
    if (np->pump) np->pump(np->ctx);
    np_ingest_last_error(m, np);
    if (np->launch_pending && np->launch_pending(np->ctx))
        np_try_launch(m);
}

static bool address_usable(const char* ip) {
    return ip && ip[0] && std::strcmp(ip, "Unavailable") != 0 &&
           std::strcmp(ip, "Detecting...") != 0;
}

static const char* disc_complaint(const LauncherModel* m) {
    return m->verify.netplay_detail[0] ? m->verify.netplay_detail
                                       : "The game disc is needed for network play.";
}

static bool hover_moved(bool hovered) {
    return hovered && (ImGui::GetIO().MouseDelta.x != 0.0f ||
                       ImGui::GetIO().MouseDelta.y != 0.0f);
}

/* ---- mode page ------------------------------------------------------------- */

/* Online play is not offered: it uses a lobby menu that isn't ours. Its
 * row stays past the end of the list, unreachable. */
enum ModeRow { MODE_LAN = 0, MODE_CALLSIGN, MODE_BACK, MODE__COUNT, MODE_ONLINE };

static void draw_net_mode(LauncherModel* m, const Gfx& g, const Input& in) {
    const auto* np = np_cb(m);
    if (!np) { launcher_model_set_view(m, LNG_VIEW_DASHBOARD); return; }
    net_setup(m);
    if (np->pump) np->pump(np->ctx);

    /* Online waits a moment for a saved sign-in to answer, as the stock page
     * does, so nobody is sent to sign in again by pressing too early. */
    const bool offered = np_account_offered(m);
    const int acct = np->account_state ? np->account_state(np->ctx)
                                       : RECOMP_LAUNCHER_ACCOUNT_GUEST;
    const double now = ImGui::GetTime();
    const bool awaiting = offered && acct == RECOMP_LAUNCHER_ACCOUNT_WAITING;
    if (!awaiting) g_state.signin_since = 0.0;
    else if (g_state.signin_since <= 0.0) g_state.signin_since = now;
    const bool hold_online = awaiting && now - g_state.signin_since <= 5.0;
    const bool signed_in = np_account_signed_in(m);

    move_cursor(PAGE_NET_MODE, MODE__COUNT, in, MODE__COUNT);
    int& cur = g_state.cursor[PAGE_NET_MODE];

    header(g, "MULTIPLAYER", "LINK SYSTEM");
    panel(g, 24.0f, 76.0f, 270.0f, 340.0f, "SELECT");
    const std::string callsign = "< " + upper(np_effective_name(m)) + " >";
    const char* labels[MODE__COUNT] = { "LAN / DIRECT IP", "CALLSIGN", "BACK" };
    int activate = -1;
    for (int i = 0; i < MODE__COUNT; ++i) {
        bool hovered = false;
        const bool enabled = !(i == MODE_ONLINE && hold_online) &&
                             !(i == MODE_CALLSIGN && signed_in);
        if (row(g, 25.0f, 90.0f + kRowH * i, 268.0f, i == cur, enabled, labels[i],
                i == MODE_CALLSIGN ? callsign.c_str() : nullptr, false, false,
                &hovered))
            activate = i;
        if (hover_moved(hovered)) cur = i;
    }
    if (in.confirm) activate = cur;

    panel(g, 310.0f, 76.0f, 306.0f, 150.0f, "STATUS");
    {
        float y = 92.0f;
        char buf[160];
        const bool disc = launcher_model_netplay_disc_ok(m);
        text(g, 324.0f, y, 11.0f, kDim, "CALLSIGN");
        text(g, 424.0f, y, 11.0f, kBright, upper(np_effective_name(m)).c_str());
        y += 19.0f;
        text(g, 324.0f, y, 11.0f, kDim, "DISC");
        text(g, 424.0f, y, 11.0f, disc ? kOn : kWarn, disc ? "READY" : "NOT READY");
        y += 19.0f;
        text(g, 324.0f, y, 11.0f, kDim, "ADDRESS");
        text(g, 424.0f, y, 11.0f, kText,
             address_usable(m->netplay_host_ip) ? m->netplay_host_ip : "NOT FOUND");
        y += 19.0f;
        text(g, 324.0f, y, 11.0f, kDim, "MEMORY CARD");
        std::snprintf(buf, sizeof buf, "%u / 15 BLOCKS",
                      (unsigned)launcher_model_memcard_blocks_used(m, 0));
        text(g, 424.0f, y, 11.0f, kText, buf);
        y += 19.0f;
        if (false && offered) {   /* online play is not offered */
            text(g, 324.0f, y, 11.0f, kDim, "ACCOUNT");
            text(g, 424.0f, y, 11.0f, signed_in ? kOn : kText,
                 signed_in ? "SIGNED IN" : awaiting ? "CHECKING" : "GUEST");
        }
    }

    static const char* const info[MODE__COUNT] = {
        "Play someone on the same network, or connect straight to an address "
        "they give you. Rooms on your network appear by themselves.",
        "The name other pilots see. Left and right pick from the arena "
        "roster; press the button to type one of your own.",
        "Return to the system menu.",
    };
    panel(g, 310.0f, 242.0f, 306.0f, 174.0f, "INFO");
    text(g, 324.0f, 258.0f, 11.0f, kHi, labels[cur]);
    text(g, 324.0f, 280.0f, 11.0f, kText,
         (cur == MODE_ONLINE && hold_online) ? "Checking your saved sign-in."
         : (cur == MODE_CALLSIGN && signed_in)
             ? "You are signed in, so your account name is used."
             : info[cur], 278.0f);
    if (!launcher_model_netplay_disc_ok(m))
        text(g, 324.0f, 372.0f, 10.0f, kWarn, disc_complaint(m), 278.0f);

    footer(g, "A / ENTER  SELECT      LEFT / RIGHT  CALLSIGN      B / ESC  BACK");

    if (cur == MODE_CALLSIGN && !signed_in && activate < 0) {
        if (in.left)  step_callsign(m, -1);
        if (in.right) step_callsign(m, 1);
    }
    if (in.back) activate = MODE_BACK;
    if (activate < 0) return;
    if (activate != MODE_BACK) cur = activate;
    switch (activate) {
        case MODE_LAN:
            g_state.next_list = 0.0;
            np_enter_netplay(m, 1);
            break;
        case MODE_ONLINE:
            if (hold_online) break;
            g_state.next_list = 0.0;
            if (!offered || signed_in) np_enter_netplay(m, 2);
            else launcher_model_set_view(m, LNG_VIEW_NETPLAY_SIGNIN);
            break;
        case MODE_CALLSIGN:
            if (!signed_in) np_open_name_modal(m);
            break;
        default:
            launcher_model_set_view(m, LNG_VIEW_DASHBOARD);
            break;
    }
}

/* ---- hosting and joining ----------------------------------------------------- */

/* One press: a room under the default name, no password, on the first free
 * port. The stock Host Lobby dialog's Create, without the dialog. */
static void host_room(LauncherModel* m) {
    const auto* np = np_cb(m);
    if (!np || !np->create) return;
    if (!launcher_model_netplay_disc_ok(m)) {
        std::snprintf(m->netplay_status, sizeof m->netplay_status, "%s",
                      disc_complaint(m));
        return;
    }
    m->netplay_lan_only = (m->netplay_mode == 1);
    if (!m->netplay_lan_only) np_connect_and_list(m);
    np_refresh_host_ip(m);
    if (!address_usable(m->netplay_host_ip)) {
        std::snprintf(m->netplay_status, sizeof m->netplay_status,
                      "No network address found. Check the connection.");
        return;
    }
    if (!m->netplay_host_name[0] || np_name_refused(m, m->netplay_host_name))
        std::snprintf(m->netplay_host_name, sizeof m->netplay_host_name,
                      "%s's Lobby", np_effective_name(m));
    m->netplay_host_password[0] = '\0';

    char endpoint[96];
    const char* port_label = np_valid_port(m->netplay_host_port)
                                 ? m->netplay_host_port : "7777";
    std::snprintf(endpoint, sizeof endpoint, "%s:%s", m->netplay_host_ip, port_label);
    const int want_port = launcher_endpoint_port(endpoint);
    const int free_port = launcher_udp_find_free_port(want_port, 32);
    if (free_port < 0 ||
        (free_port != want_port &&
         launcher_endpoint_set_port(endpoint, sizeof endpoint, free_port) != 0)) {
        std::snprintf(m->netplay_status, sizeof m->netplay_status,
                      "No free network port near %s. Try again.", port_label);
        return;
    }
    const int max_slots = np_clamp_host_max_players(m);
    if (np->rollback_set)
        (void)np->rollback_set(np->ctx, m->netplay_rollback ? 1 : 0);
    if (np->input_prediction_set)
        (void)np->input_prediction_set(np->ctx, m->netplay_lobby_input_prediction);
    if (np->input_delay_set)
        (void)np->input_delay_set(np->ctx, m->netplay_lobby_input_delay);
    const int rc = np->create(np->ctx, m->netplay_host_name, endpoint,
                              m->netplay_host_password, &m->s,
                              m->netplay_lan_only ? 1 : 0, max_slots);
    if (rc != 0) {
        std::snprintf(m->netplay_status, sizeof m->netplay_status,
                      rc == -4 ? "That network port is in use. Try again."
                               : "Could not open a room.");
        return;
    }
    if (const char* colon = std::strrchr(endpoint, ':'))
        std::snprintf(m->netplay_host_port, sizeof m->netplay_host_port, "%s",
                      colon + 1);
    std::snprintf(m->netplay_host_endpoint, sizeof m->netplay_host_endpoint, "%s",
                  endpoint);
    std::snprintf(m->netplay_host_local_ip, sizeof m->netplay_host_local_ip, "%s",
                  m->netplay_host_ip);
    np_save_network_settings(m);
    m->netplay_lobby_max_slots = max_slots;
    m->netplay_local_room = m->netplay_lan_only || !np_connected(m);
    m->netplay_status[0] = '\0';
}

/* The stock Host Lobby dialog, for a room name or a password. */
static void host_room_dialog(LauncherModel* m) {
    if (!launcher_model_netplay_disc_ok(m)) {
        std::snprintf(m->netplay_status, sizeof m->netplay_status, "%s",
                      disc_complaint(m));
        return;
    }
    m->netplay_lan_only = (m->netplay_mode == 1);
    if (!m->netplay_lan_only) np_connect_and_list(m);
    np_refresh_host_ip(m);
    if (!m->netplay_host_name[0])
        std::snprintf(m->netplay_host_name, sizeof m->netplay_host_name,
                      "%s's Lobby", np_effective_name(m));
    m->netplay_host_password[0] = '\0';
    m->netplay_host_modal_open = true;
}

/* ---- recent hosts ----------------------------------------------------------------- */

/* Addresses joined before, newest first, kept beside the other network
 * settings. They are offered in the room list so a repeat connection is one
 * press, never a second round of dialling. */
static const size_t kRecentMax = 4;

static std::string recent_path() { return asset("saves/netplay/recent_hosts.txt"); }

static bool endpoint_ok(const std::string& e) {
    int a = -1, b = -1, c = -1, d = -1, port = -1;
    char tail = 0;
    if (std::sscanf(e.c_str(), "%d.%d.%d.%d:%d%c", &a, &b, &c, &d, &port, &tail) != 5)
        return false;
    return a >= 0 && a <= 255 && b >= 0 && b <= 255 && c >= 0 && c <= 255 &&
           d >= 0 && d <= 255 && port >= 1 && port <= 65535;
}

static void recent_load() {
    if (g_state.recent_loaded) return;
    g_state.recent_loaded = true;
    g_state.recent.clear();
    FILE* f = std::fopen(recent_path().c_str(), "rb");
    if (!f) return;
    char line[128];
    while (std::fgets(line, sizeof line, f) && g_state.recent.size() < kRecentMax) {
        std::string e(line);
        while (!e.empty() && (e.back() == '\n' || e.back() == '\r' || e.back() == ' '))
            e.pop_back();
        if (endpoint_ok(e) &&
            std::find(g_state.recent.begin(), g_state.recent.end(), e) == g_state.recent.end())
            g_state.recent.push_back(e);
    }
    std::fclose(f);
}

static void recent_save() {
    std::error_code ec;
    const std::filesystem::path file = std::filesystem::u8path(recent_path());
    std::filesystem::create_directories(file.parent_path(), ec);
    FILE* f = std::fopen(recent_path().c_str(), "wb");
    if (!f) return;
    for (const std::string& e : g_state.recent) std::fprintf(f, "%s\n", e.c_str());
    std::fclose(f);
}

static void recent_add(const char* endpoint) {
    recent_load();
    const std::string e(endpoint ? endpoint : "");
    if (!endpoint_ok(e)) return;
    g_state.recent.erase(std::remove(g_state.recent.begin(), g_state.recent.end(), e),
                         g_state.recent.end());
    g_state.recent.insert(g_state.recent.begin(), e);
    if (g_state.recent.size() > kRecentMax) g_state.recent.resize(kRecentMax);
    recent_save();
}

static void recent_forget(const std::string& e) {
    g_state.recent.erase(std::remove(g_state.recent.begin(), g_state.recent.end(), e),
                         g_state.recent.end());
    recent_save();
}

/* ---- join by address ----------------------------------------------------------- */

/* The address is edited a digit at a time: twelve for the four numbers, five
 * for the port. No digit is more than five presses from any other. */
static const int kDirectDigits = 17;

static void direct_open(LauncherModel* m) {
    int a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(m->netplay_direct_ip, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) {
        /* Start from this machine's own network: usually only the last
         * number differs. */
        if (std::sscanf(m->netplay_host_ip, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) {
            a = 192; b = 168; c = 1;
        }
        d = 1;
    }
    const int ip[4] = { a, b, c, d };
    for (int i = 0; i < 4; ++i)
        g_state.direct_ip[i] = ip[i] < 0 ? 0 : ip[i] > 255 ? 255 : ip[i];
    g_state.direct_port = np_valid_port(m->netplay_direct_port)
                              ? std::atoi(m->netplay_direct_port) : 7777;
    g_state.direct_digit = 9;   /* first digit of the last number */
    g_state.direct = true;
    m->netplay_status[0] = '\0';
}

/* Which number a digit belongs to (0-3 address, 4 port) and what it is worth. */
static int direct_field_of(int digit) { return digit < 12 ? digit / 3 : 4; }
static int direct_place_of(int digit) {
    static const int three[3] = { 100, 10, 1 };
    static const int five[5] = { 10000, 1000, 100, 10, 1 };
    return digit < 12 ? three[digit % 3] : five[digit - 12];
}
static int& direct_value_of(int digit) {
    const int f = direct_field_of(digit);
    return f < 4 ? g_state.direct_ip[f] : g_state.direct_port;
}
static int direct_digit_get(int digit) {
    return (direct_value_of(digit) / direct_place_of(digit)) % 10;
}
static void direct_digit_put(int digit, int n) {
    int& v = direct_value_of(digit);
    const int place = direct_place_of(digit);
    v += (n - (v / place) % 10) * place;
    if (direct_field_of(digit) < 4) {
        if (v > 255) v = 255;
    } else {
        if (v > 65535) v = 65535;
        if (v < 1) v = 1;
    }
}
static void direct_digit_step(int digit, int step) {
    /* The leading digit of a number only runs as far as the number can go. */
    const int f = direct_field_of(digit);
    const int place = direct_place_of(digit);
    const int span = (f < 4 && place == 100) ? 3 : (f == 4 && place == 10000) ? 7 : 10;
    const int now = direct_digit_get(digit) % span;
    direct_digit_put(digit, ((now + step) % span + span) % span);
}

static void join_address(LauncherModel* m, const char* ip, const char* port) {
    const auto* np = np_cb(m);
    if (!np || !np->join) return;
    if (!launcher_model_netplay_disc_ok(m)) {
        std::snprintf(m->netplay_status, sizeof m->netplay_status, "%s",
                      disc_complaint(m));
        return;
    }
    char lobby_id[96];
    char guest_bind[64];
    std::snprintf(lobby_id, sizeof lobby_id, "lan:%s:%s", ip, port);
    if (!np_prepare_guest_bind(guest_bind, sizeof guest_bind, m->netplay_status,
                               sizeof m->netplay_status))
        return;
    const int rc = np->join(np->ctx, lobby_id, "", guest_bind);
    if (rc == 0) {
        m->netplay_local_room = true;
        std::snprintf(m->netplay_host_endpoint, sizeof m->netplay_host_endpoint,
                      "%s", lobby_id + 4);
        m->netplay_lobby_max_slots = np_game_max_players(m, true);
        m->netplay_status[0] = '\0';
        g_state.direct = false;
        recent_add(lobby_id + 4);
    } else if (rc == -2) {
        std::snprintf(m->netplay_status, sizeof m->netplay_status,
                      "That room has a password. Use JOIN BY ADDRESS, then "
                      "TYPE IT, to enter one.");
    } else if (rc == -3) {
        std::snprintf(m->netplay_status, sizeof m->netplay_status,
                      "No answer from that address. Check that the host has a "
                      "room open and the numbers are right.");
    } else {
        std::snprintf(m->netplay_status, sizeof m->netplay_status,
                      "Could not join: the room is full, already playing, or "
                      "running a different version.");
    }
}

static void direct_store(LauncherModel* m) {
    std::snprintf(m->netplay_direct_ip, sizeof m->netplay_direct_ip, "%d.%d.%d.%d",
                  g_state.direct_ip[0], g_state.direct_ip[1],
                  g_state.direct_ip[2], g_state.direct_ip[3]);
    std::snprintf(m->netplay_direct_port, sizeof m->netplay_direct_port, "%d",
                  g_state.direct_port);
}

static void direct_join(LauncherModel* m) {
    direct_store(m);
    join_address(m, m->netplay_direct_ip, m->netplay_direct_port);
}

static void draw_net_direct(LauncherModel* m, const Gfx& g, const Input& in) {
    int& digit = g_state.direct_digit;
    if (digit < 0 || digit >= kDirectDigits) digit = 9;
    if (in.left)  digit = (digit + kDirectDigits - 1) % kDirectDigits;
    if (in.right) digit = (digit + 1) % kDirectDigits;
    if (in.up)    direct_digit_step(digit, 1);
    if (in.down)  direct_digit_step(digit, -1);
    /* Shoulder buttons: the first digit of the next or the previous number. */
    static const int field_start[5] = { 0, 3, 6, 9, 12 };
    if (in.page_up)   digit = field_start[(direct_field_of(digit) + 1) % 5];
    if (in.page_down) digit = field_start[(direct_field_of(digit) + 4) % 5];
    /* A keyboard types straight in. */
    if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                     ImGuiPopupFlags_AnyPopupLevel)) {
        for (int n = 0; n < 10; ++n) {
            if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_0 + n), false) ||
                ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_Keypad0 + n), false)) {
                direct_digit_put(digit, n);
                digit = (digit + 1) % kDirectDigits;
                break;
            }
        }
    }

    header(g, "JOIN BY ADDRESS", "LAN PLAY");
    panel(g, 24.0f, 76.0f, 592.0f, 190.0f, "HOST ADDRESS");
    {
        /* One line of seventeen digit cells, centred, the chosen one boxed. */
        const float size = 30.0f;
        float cell = 0.0f;
        for (char c = '0'; c <= '9'; ++c) {
            const char one[2] = { c, 0 };
            cell = std::max(cell, text_width(g, size, one));
        }
        cell += 5.0f;
        const float dot_w = text_width(g, size, ".") + 10.0f;
        const float colon_w = text_width(g, size, ":") + 16.0f;
        float x = (kW - (cell * kDirectDigits + dot_w * 3.0f + colon_w)) * 0.5f;
        const float y = 140.0f;
        for (int i = 0; i < kDirectDigits; ++i) {
            const bool sel = i == digit;
            const char one[2] = { (char)('0' + direct_digit_get(i)), 0 };
            const ImVec2 a = at(g, x, y - 6.0f), b = at(g, x + cell, y + size + 6.0f);
            if (ImGui::IsMouseHoveringRect(a, b, false) &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                digit = i;
            if (sel) {
                g.dl->AddRectFilled(a, b, IM_COL32(240, 160, 32, 50));
                g.dl->AddRect(a, b, kHi);
                const float cx = x + cell * 0.5f;
                g.dl->AddTriangleFilled(at(g, cx - 6.0f, y - 12.0f),
                                        at(g, cx + 6.0f, y - 12.0f),
                                        at(g, cx, y - 20.0f), kHi);
                g.dl->AddTriangleFilled(at(g, cx - 6.0f, y + size + 12.0f),
                                        at(g, cx + 6.0f, y + size + 12.0f),
                                        at(g, cx, y + size + 20.0f), kHi);
            }
            /* The other digits of the number being edited stay lit. */
            const bool same = direct_field_of(i) == direct_field_of(digit);
            text(g, x + (cell - text_width(g, size, one)) * 0.5f, y, size,
                 sel ? kBright : same ? kText : kDim, one);
            x += cell;
            if (i == 2 || i == 5 || i == 8) {
                text(g, x + 5.0f, y, size, kDim, ".");
                x += dot_w;
            } else if (i == 11) {
                text(g, x + 8.0f, y, size, kDim, ":");
                x += colon_w;
            }
        }
        text(g, 44.0f, 232.0f, 10.0f, kDim, "ADDRESS");
        text_right(g, 596.0f, 232.0f, 10.0f, kDim, "PORT");
    }

    panel(g, 24.0f, 282.0f, 592.0f, 134.0f, "INFO");
    text(g, 38.0f, 298.0f, 11.0f, kText,
         "Enter the address shown on the host's room screen, one digit at a "
         "time. Left and right choose a digit, up and down change it, and the "
         "shoulder buttons jump to the next number. The address is remembered "
         "once you have joined.", 564.0f);
    if (address_usable(m->netplay_host_ip)) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "THIS MACHINE  %s", m->netplay_host_ip);
        text(g, 38.0f, 350.0f, 10.0f, kDim, buf);
    }
    if (m->netplay_status[0])
        text(g, 38.0f, 370.0f, 11.0f, kWarn, m->netplay_status, 564.0f);

    footer(g, "A  CONNECT      D-PAD  SET      LB / RB  NEXT NUMBER      X  TYPE IT      B  BACK");

    if (in.confirm || in.start) {
        direct_join(m);
    } else if (in.alt) {
        /* The stock dialog: a keyboard, and room for a password. */
        direct_store(m);
        m->netplay_password[0] = '\0';
        m->netplay_status[0] = '\0';
        m->netplay_direct_modal_open = true;
        g_state.direct = false;
    } else if (in.back) {
        g_state.direct = false;
        m->netplay_status[0] = '\0';
    }
}

/* ---- room list ------------------------------------------------------------------- */

static void draw_net_rooms(LauncherModel* m, const Gfx& g, const Input& in) {
    const auto* np = np_cb(m);
    if (!np) { launcher_model_set_view(m, LNG_VIEW_DASHBOARD); return; }
    net_setup(m);
    const bool lan = m->netplay_mode == 1;
    if (!m->netplay_list_fresh) np_refresh_lobby_list(m);
    net_tick(m, np);
    if (np->list_scope_set)
        np->list_scope_set(np->ctx, lan ? RECOMP_LAUNCHER_LIST_SCOPE_LAN
                                        : RECOMP_LAUNCHER_LIST_SCOPE_ONLINE);

    const bool online = np->connected && np->connected(np->ctx);
    const bool connecting = np->connecting && np->connecting(np->ctx);
    if (std::strncmp(m->netplay_status, "Connecting", 10) == 0) {
        if (online) m->netplay_status[0] = '\0';
        else if (!connecting && m->netplay_list_fresh)
            std::snprintf(m->netplay_status, sizeof m->netplay_status,
                          "Could not reach lobby server.");
    }
    /* Rooms come and go; ask again every couple of seconds so nobody has to
     * press a refresh button to see a friend's room appear. */
    const double now = ImGui::GetTime();
    if (now >= g_state.next_list) {
        g_state.next_list = now + 2.0;
        if (np->request_list && (lan || online)) np->request_list(np->ctx);
    }
    np_push_blocks(m);

    if (g_state.direct) { draw_net_direct(m, g, in); return; }

    std::vector<RecompLauncherCNetplayLobby> rooms;
    std::vector<int> room_index;   /* the backend's index for each */
    {
        const int n = np->list_count ? np->list_count(np->ctx) : 0;
        for (int i = 0; i < n; ++i) {
            RecompLauncherCNetplayLobby r;
            std::memset(&r, 0, sizeof r);
            if (!np->list_get || !np->list_get(np->ctx, i, &r)) continue;
            rooms.push_back(r);
            room_index.push_back(i);
        }
    }

    /* Addresses joined before, for LAN play, unless the room is already in
     * the list under its own name. */
    std::vector<std::string> recents;
    if (lan) {
        recent_load();
        for (const std::string& e : g_state.recent) {
            bool listed = false;
            for (const RecompLauncherCNetplayLobby& r : rooms)
                if (std::strncmp(r.lobby_id, "lan:", 4) == 0 && e == r.lobby_id + 4)
                    listed = true;
            if (!listed) recents.push_back(e);
        }
    }

    /* One cursor: HOST A ROOM, the rooms, the recent addresses, then the
     * fixed rows. */
    enum Extra { EX_DIRECT = 0, EX_OPTIONS, EX_CLASSIC, EX__COUNT };
    int extras[EX__COUNT];
    int ne = 0;
    if (lan) extras[ne++] = EX_DIRECT;
    extras[ne++] = EX_OPTIONS;
    extras[ne++] = EX_CLASSIC;
    const int nr = (int)rooms.size();
    const int nq = (int)recents.size();
    const int nl = 1 + nr + nq;
    const int count = nl + ne;
    const int visible = 7;
    {
        int& c = g_state.cursor[PAGE_NET_ROOMS];
        int& t = g_state.top[PAGE_NET_ROOMS];
        if (in.up)   c = (c + count - 1) % count;
        if (in.down) c = (c + 1) % count;
        if (c < 0) c = 0;
        if (c >= count) c = count - 1;
        if (c < nl) {
            if (c < t) t = c;
            if (c >= t + visible) t = c - visible + 1;
        }
        if (t > nl - visible) t = nl - visible;
        if (t < 0) t = 0;
    }
    int& cur = g_state.cursor[PAGE_NET_ROOMS];
    const int top = g_state.top[PAGE_NET_ROOMS];
    const bool disc = launcher_model_netplay_disc_ok(m);

    header(g, lan ? "LAN PLAY" : "ONLINE PLAY", "ROOM SELECT");
    panel(g, 24.0f, 76.0f, 340.0f, 206.0f, "ROOMS");
    int clicked = -1;
    for (int i = top; i < nl && i < top + visible; ++i) {
        bool hovered = false;
        std::string label;
        char value[32] = ">";
        if (i == 0) {
            label = "HOST A ROOM";
        } else if (i > nr) {
            label = recents[i - 1 - nr];
            std::snprintf(value, sizeof value, "RECENT >");
        } else {
            const RecompLauncherCNetplayLobby& r = rooms[i - 1];
            label = upper(r.name[0] ? r.name : "Unnamed room");
            if (label.size() > 24) label = label.substr(0, 23) + ".";
            std::snprintf(value, sizeof value, "%s%d / %d",
                          r.has_password ? "LOCK  " : "", r.player_count, r.max_slots);
        }
        if (row(g, 25.0f, 86.0f + kRowH * (i - top), 338.0f, i == cur, disc,
                label.c_str(), value, false, false, &hovered))
            clicked = i;
        if (hover_moved(hovered)) cur = i;
    }
    if (nr == 0 && nq < 6)
        text(g, 55.0f, 96.0f + kRowH * (1 + nq), 11.0f, kDim,
             !lan && !online ? (connecting ? "CONNECTING TO THE LOBBY SERVER"
                                           : "NOT CONNECTED")
                             : std::fmod(now, 1.6) < 0.8 ? "SEARCHING FOR ROOMS"
                                                         : "SEARCHING FOR ROOMS .");
    if (nl > top + visible) text(g, 55.0f, 268.0f, 9.0f, kDim, "MORE BELOW");
    if (top > 0) text_right(g, 350.0f, 268.0f, 9.0f, kDim, "MORE ABOVE");

    panel(g, 24.0f, 298.0f, 340.0f, 118.0f, "OTHER");
    static const char* const extra_label[EX__COUNT] = {
        "JOIN BY ADDRESS", "HOST WITH OPTIONS", "FULL NETWORK PAGE" };
    for (int i = 0; i < ne; ++i) {
        bool hovered = false;
        if (row(g, 25.0f, 306.0f + kRowH * i, 338.0f, nl + i == cur,
                extras[i] == EX_CLASSIC || disc, extra_label[extras[i]], ">",
                false, false, &hovered))
            clicked = nl + i;
        if (hover_moved(hovered)) cur = nl + i;
    }

    panel(g, 380.0f, 76.0f, 236.0f, 340.0f, "DETAIL");
    if (cur == 0) {
        text(g, 394.0f, 92.0f, 11.0f, kHi, "HOST A ROOM");
        text(g, 394.0f, 114.0f, 11.0f, kText,
             lan ? "Open a room on this network. Other pilots here see it in "
                   "their list within a few seconds. You are player 1."
                 : "Open a room on the lobby server for another pilot to "
                   "join. You are player 1.", 208.0f);
    } else if (cur > nr && cur < nl) {
        text(g, 394.0f, 92.0f, 11.0f, kHi, "RECENT ADDRESS");
        text(g, 394.0f, 114.0f, 11.0f, kBright, recents[cur - 1 - nr].c_str());
        text(g, 394.0f, 140.0f, 11.0f, kText,
             "A host you have joined before. Connects straight to it; the "
             "host must have a room open.", 208.0f);
    } else if (cur < nl) {
        const RecompLauncherCNetplayLobby& r = rooms[cur - 1];
        char buf[96];
        text(g, 394.0f, 92.0f, 11.0f, kHi,
             upper(r.name[0] ? r.name : "Unnamed room").c_str(), 208.0f);
        float y = 128.0f;
        text(g, 394.0f, y, 10.0f, kDim, "PILOTS");
        std::snprintf(buf, sizeof buf, "%d OF %d", r.player_count, r.max_slots);
        text(g, 474.0f, y, 10.0f, kText, buf);
        y += 17.0f;
        text(g, 394.0f, y, 10.0f, kDim, "PASSWORD");
        text(g, 474.0f, y, 10.0f, r.has_password ? kHi : kText,
             r.has_password ? "NEEDED" : "NONE");
        y += 17.0f;
        text(g, 394.0f, y, 10.0f, kDim, "DELAY");
        if (r.latency_ms >= 0) std::snprintf(buf, sizeof buf, "%d MS", r.latency_ms);
        else std::snprintf(buf, sizeof buf, "-");
        text(g, 474.0f, y, 10.0f, kText, buf);
        y += 17.0f;
        if (std::strncmp(r.lobby_id, "lan:", 4) == 0) {
            text(g, 394.0f, y, 10.0f, kDim, "ADDRESS");
            text(g, 474.0f, y, 10.0f, kText, r.lobby_id + 4);
            y += 17.0f;
        }
        text(g, 394.0f, y + 10.0f, 11.0f, kText,
             r.player_count >= r.max_slots
                 ? "This room is full."
                 : "Join as player 2. You bring an AC from the room's garage.",
             208.0f);
    } else {
        const int ex = extras[cur - nl];
        text(g, 394.0f, 92.0f, 11.0f, kHi, extra_label[ex]);
        text(g, 394.0f, 114.0f, 11.0f, kText,
             ex == EX_DIRECT
                 ? "Dial a host's address number by number, for a room that "
                   "does not show in the list."
             : ex == EX_OPTIONS
                 ? "Host with a room name or a password of your choosing. "
                   "Needs a keyboard."
                 : "The full network page: match settings, chat and the "
                   "rest. Needs a mouse or a keyboard.", 208.0f);
    }
    if (m->netplay_status[0])
        text(g, 394.0f, 300.0f, 10.0f, kWarn, m->netplay_status, 208.0f);
    else if (!disc)
        text(g, 394.0f, 300.0f, 10.0f, kWarn, disc_complaint(m), 208.0f);
    {
        char buf[96];
        std::snprintf(buf, sizeof buf, "CALLSIGN  %s",
                      upper(np_effective_name(m)).c_str());
        text(g, 394.0f, 380.0f, 10.0f, kDim, buf);
        std::snprintf(buf, sizeof buf, "ROOMS %02d", nr);
        text_right(g, 604.0f, 396.0f, 10.0f, kDim, buf);
        if (lan && address_usable(m->netplay_host_ip)) {
            std::snprintf(buf, sizeof buf, "ADDRESS  %s", m->netplay_host_ip);
            text(g, 394.0f, 396.0f, 10.0f, kDim, buf);
        }
    }
    const bool on_recent = cur > nr && cur < nl;
    footer(g, on_recent ? "A / ENTER  CONNECT      X  FORGET      B / ESC  BACK"
                        : "A / ENTER  SELECT      X  REFRESH      B / ESC  BACK");

    if (clicked >= 0) cur = clicked;
    if (in.confirm || clicked >= 0) {
        if (cur == 0) {
            host_room(m);
        } else if (cur > nr && cur < nl) {
            const std::string e = recents[cur - 1 - nr];
            const size_t colon = e.rfind(':');
            join_address(m, e.substr(0, colon).c_str(), e.substr(colon + 1).c_str());
        } else if (cur < nl) {
            if (!disc) {
                std::snprintf(m->netplay_status, sizeof m->netplay_status, "%s",
                              disc_complaint(m));
            } else {
                const RecompLauncherCNetplayLobby& r = rooms[cur - 1];
                m->netplay_selected_lobby = room_index[cur - 1];
                m->netplay_status[0] = '\0';
                np_join_selected(m);
                /* Joined a room on this network: remember where it was. */
                if (std::strncmp(r.lobby_id, "lan:", 4) == 0 && m->netplay_local_room &&
                    std::strcmp(m->netplay_host_endpoint, r.lobby_id + 4) == 0)
                    recent_add(r.lobby_id + 4);
            }
        } else {
            switch (extras[cur - nl]) {
                case EX_DIRECT:  direct_open(m); break;
                case EX_OPTIONS: host_room_dialog(m); break;
                default:         g_state.classic_net = true; break;
            }
        }
    } else if (in.alt && on_recent) {
        recent_forget(recents[cur - 1 - nr]);
        note("FORGOTTEN");
    } else if (in.alt) {
        np_refresh_lobby_list(m);
        g_state.next_list = now + 2.0;
        note("SEARCHING");
    } else if (in.back) {
        launcher_model_set_view(m, LNG_VIEW_NETPLAY_MODE);
    }
}

/* ---- the room ----------------------------------------------------------------------
 *
 * The versus room. Each pilot brings an AC, the host sets the match, and the
 * match starts by itself once both pilots are ready.
 *
 *  - Your AC: loaded from a Master of Arena save on one of your memory cards,
 *    or from an AI card, then changed in the garage's ASSEMBLY, OPTIONAL
 *    PARTS, PAINT and EMBLEM screens. With the host's FULL GARAGE off (the
 *    default) only your own parts can be fitted: the parts the save you
 *    loaded owns (as the game's garage lists them) and those the AC came
 *    with; HUMAN PLUS stays as loaded. FULL GARAGE on unlocks every part and
 *    the EXTRA GARAGE AC. Kept between runs (RecompData/garage/versus-ac.png,
 *    with where it came from and your parts in versus-ac.txt beside it).
 *  - READY puts your AC alone on a match-only memory card
 *    (PSX_MEMCARD1_NETPLAY; your own cards are never touched): the host's is
 *    the match's slot 1, the guest's is uploaded as slot 2. The room keeps
 *    the guest card offer on, and each pilot's card "share" bit is that
 *    pilot's READY, which the seat table carries to the other side. The
 *    other pilot's AC is never shown here: the game's VS screen introduces
 *    both (games/master-of-arena/moa_netplay_room.c).
 *  - MATCH SETTINGS (host): stage, time limit, rounds, full garage, loading.
 *    Sent to the guest while the room waits (room_opts_set) and settled at
 *    START as the match options (PSX_NET_GAME_OPTS) that the game reads.
 *  - Both ready: a three-second count, then the host starts the match. Either
 *    pilot leaving READY stops it; the host can't change the settings while
 *    ready, and a guest who readied is un-readied when they change.
 *  - After the set both pilots are back here (the game ends the session);
 *    READY again for the next one.
 */

struct RoomStage { int id; const char* name; };
/* Green Silo first (the room's default); the arena's main stages in the
 * order the user picked for Rogue Arena. The link engine (netplay) loads
 * every stage 20..39 (claude/netplay-room-rework-handover.md); these are the
 * named ones. RANDOM is settled to one of them when the match starts. */
static const RoomStage kRoomStages[] = {
    { 27, "GREEN SILO" }, { 39, "OCEAN PLATFORM" }, { 29, "SEABED" }, { 36, "NIGHT CITY" },
    { 25, "GARDEN PLAZA" }, { 20, "LUNAR BASE" }, { 22, "PILLAR HALL" }, { 24, "SCAFFOLD YARD" },
    { 33, "FOREST" }, { 0, "RANDOM" },
};
static const int kRoomStageCount = (int)(sizeof kRoomStages / sizeof kRoomStages[0]);
/* The game's own VS TIME LIMIT choices: options byte 0 = 2:00, 1 = 5:00, 2 = none. */
static const char* const kRoomTimes[3] = { "2:00", "5:00", "NO LIMIT" };
static const int kRoomRounds[3] = { 1, 3, 5 };

struct RoomOpts {
    int  stage = 27;       /* stage id, 0 = RANDOM */
    int  time = 0;         /* VS TIME LIMIT option: 0 2:00, 1 5:00, 2 none */
    int  rounds = 1;       /* 1, 3 or 5: first to (rounds + 1) / 2 wins */
    bool garage = false;   /* FULL GARAGE: every part, the builder screens */
    bool fast = true;      /* both PCs load at the fast drive speed */
};

static std::string room_opts_text(const RoomOpts& o, int stage) {
    char b[96];
    std::snprintf(b, sizeof b, "room=1;stage=%d;time=%d;rounds=%d;garage=%d;fastdisc=%d",
                  stage, o.time, o.rounds, o.garage ? 1 : 0, o.fast ? 16 : 0);
    return b;
}

static bool room_opts_parse(const char* s, RoomOpts* o) {
    if (!s || std::strncmp(s, "room=1", 6) != 0) return false;
    RoomOpts r;
    for (const char* p = s; p && *p;) {
        const char* eq = std::strchr(p, '=');
        const char* semi = std::strchr(p, ';');
        if (eq && (!semi || eq < semi)) {
            const std::string k(p, (size_t)(eq - p));
            const int v = std::atoi(eq + 1);
            if (k == "stage") r.stage = v;
            else if (k == "time") r.time = v < 0 || v > 2 ? 0 : v;
            else if (k == "rounds") r.rounds = v == 3 || v == 5 ? v : 1;
            else if (k == "garage") r.garage = v != 0;
            else if (k == "fastdisc") r.fast = v > 0;
        }
        p = semi ? semi + 1 : nullptr;
    }
    *o = r;
    return true;
}

/* Same match as readied: a RANDOM stage settled to one is still the same. */
static bool room_opts_same(const RoomOpts& readied, const RoomOpts& now) {
    return (readied.stage == now.stage || readied.stage == 0) && readied.time == now.time &&
           readied.rounds == now.rounds && readied.garage == now.garage && readied.fast == now.fast;
}

static const char* room_stage_name(int id) {
    for (int i = 0; i < kRoomStageCount; ++i)
        if (kRoomStages[i].id == id) return kRoomStages[i].name;
    return "STAGE";
}

static const char* room_rounds_label(int rounds) {
    return rounds == 5 ? "BEST OF 5" : rounds == 3 ? "BEST OF 3" : "SINGLE";
}

enum RoomView { RV_MAIN = 0, RV_GARAGE, RV_SAVES, RV_CARDS, RV_SETTINGS, RV_BUILDER };

static RoomOpts g_room;                 /* host: its choices; guest: the host's as heard */
static RoomOpts g_room_ready_opts;      /* the options when this pilot readied */
static bool   g_room_heard = false;     /* guest: the host's options have arrived */
static int    g_room_view = RV_MAIN;
static bool   g_room_ready = false;
static double g_room_count_at = 0.0;    /* both ready since (ImGui time); 0 = no count */
static bool   g_room_started = false;   /* host: START sent for this count */
static bool   g_room_launched = false;  /* a match was launched from here */
static std::chrono::steady_clock::time_point g_room_launch_at;
static std::string g_room_result;       /* the last set, as the game reported it */

/* ---- the room's AC ---- */

static moa_card    g_room_ac;
static bool        g_room_ac_read = false;
static bool        g_room_has_ac = false;
static std::string g_room_ac_from;       /* "SAVE ...", "AI CARD", "EXTRA GARAGE" */
static bool        g_room_ac_edited = false;   /* changed in the garage */

/* Your own parts (FULL GARAGE off): the owned-part counts of the save you
 * loaded last (moa_owned_index numbering), and the AC as it was loaded: an
 * AI card's parts come with it. */
static uint8_t     g_room_owned[256];
static bool        g_room_owned_known = false;
static int         g_room_owned_count = 0;     /* different parts owned */
static std::string g_room_owned_from;          /* "CARD  BLOCK n" */
static uint8_t     g_room_owned_plus = 0;      /* that save's HUMAN PLUS */
static uint8_t     g_room_base[13];
static uint8_t     g_room_base_plus = 0;

static std::string room_ac_path() { return asset("garage/versus-ac.png"); }
static std::string room_ac_note_path() { return asset("garage/versus-ac.txt"); }
static std::string room_card_path() { return asset("saves/netplay/versus-card.mcd"); }
static std::string room_result_path() { return asset("saves/netplay/versus-result.txt"); }

static void room_ac_read() {
    if (g_room_ac_read) return;
    g_room_ac_read = true;
    std::vector<uint8_t> data;
    if (FILE* f = std::fopen(room_ac_path().c_str(), "rb")) {
        if (std::fseek(f, 0, SEEK_END) == 0) {
            const long n = std::ftell(f);
            if (n > 0 && n < 32L * 1024 * 1024 && std::fseek(f, 0, SEEK_SET) == 0) {
                data.resize((size_t)n);
                if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
            }
        }
        std::fclose(f);
    }
    g_room_has_ac = !data.empty() && moa_card_decode(data.data(), data.size(), &g_room_ac);
    if (!g_room_has_ac) return;
    std::memcpy(g_room_base, g_room_ac.rec, sizeof g_room_base);
    g_room_base_plus = g_room_ac.plus;
    if (FILE* f = std::fopen(room_ac_note_path().c_str(), "rb")) {
        char line[640] = {0};
        auto trimmed = [&]() {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            return s;
        };
        auto hex = [](const char* p, uint8_t* out, size_t n) {
            for (size_t i = 0; i < n; ++i) {
                unsigned v = 0;
                if (std::sscanf(p + 2 * i, "%2x", &v) != 1) return false;
                out[i] = (uint8_t)v;
            }
            return true;
        };
        if (std::fgets(line, sizeof line, f)) g_room_ac_from = trimmed();
        if (std::fgets(line, sizeof line, f)) g_room_ac_edited = std::atoi(line) != 0;
        while (std::fgets(line, sizeof line, f)) {
            const std::string l = trimmed();
            if (l.rfind("base=", 0) == 0 && l.size() >= 5 + 26) {
                uint8_t b[13];
                if (hex(l.c_str() + 5, b, 13)) {
                    std::memcpy(g_room_base, b, 13);
                    const size_t comma = l.find(',');
                    g_room_base_plus = comma != std::string::npos ? (uint8_t)std::atoi(l.c_str() + comma + 1) : 0;
                }
            } else if (l.rfind("parts=", 0) == 0) {
                g_room_owned_from = l.substr(6);
            } else if (l.rfind("plus=", 0) == 0) {
                g_room_owned_plus = (uint8_t)std::atoi(l.c_str() + 5);
            } else if (l.rfind("owned=", 0) == 0 && l.size() >= 6 + 512) {
                if (hex(l.c_str() + 6, g_room_owned, 256)) {
                    g_room_owned_known = true;
                    g_room_owned_count = 0;
                    for (int i = 0; i < 256; ++i) g_room_owned_count += g_room_owned[i] != 0;
                }
            }
        }
        std::fclose(f);
    }
}

static void room_ac_save() {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(room_ac_path()).parent_path(), ec);
    if (!g_room_ac.ac[0]) std::strcpy(g_room_ac.ac, "AC");
    if (!moa_card_write_png(&g_room_ac, room_ac_path().c_str())) {
        note("YOUR AC COULD NOT BE KEPT");
        return;
    }
    if (FILE* f = std::fopen(room_ac_note_path().c_str(), "wb")) {
        std::fprintf(f, "%s\n%d\nbase=", g_room_ac_from.c_str(), g_room_ac_edited ? 1 : 0);
        for (int i = 0; i < 13; ++i) std::fprintf(f, "%02X", g_room_base[i]);
        std::fprintf(f, ",%u\n", (unsigned)g_room_base_plus);
        if (g_room_owned_known) {
            std::fprintf(f, "parts=%s\nplus=%u\nowned=", g_room_owned_from.c_str(), (unsigned)g_room_owned_plus);
            for (int i = 0; i < 256; ++i) std::fprintf(f, "%02X", g_room_owned[i]);
            std::fprintf(f, "\n");
        }
        std::fclose(f);
    }
}

static void room_ac_set(const moa_card& c, const std::string& from) {
    g_room_ac = c;
    moa_build_normalise(g_room_ac.rec);
    g_room_has_ac = true;
    g_room_ac_from = from;
    g_room_ac_edited = false;
    std::memcpy(g_room_base, g_room_ac.rec, sizeof g_room_base);   /* the AC as it came */
    g_room_base_plus = g_room_ac.plus;
    room_ac_save();
}

/* ---- your own parts (FULL GARAGE off) ---- */

static bool room_own_parts_only() { return !g_room.garage; }

/* What an AC came with counts as yours, except the EXTRA GARAGE AC's (that
 * garage has every part): its parts have to be ones your save owns. */
static bool room_base_counts() { return g_room_ac_from != "EXTRA GARAGE"; }

/* May `rec` hold part `id` in record byte `byte`? With FULL GARAGE off: no
 * part at all, or a part you have, where you have the most of what your
 * save owns and what the AC came with. The same back weapon on both sides
 * takes two (as the game's garage). */
static bool room_part_ok_in(const uint8_t* rec, int byte, int id) {
    if (!room_own_parts_only()) return true;
    if (id == 255 || (byte == 4 && id == 0)) return true;
    const int k = moa_owned_index(byte, id);
    if (k < 0 || byte < 0 || byte > 12) return false;
    const bool back = byte == 6 || byte == 7;
    int have = !room_base_counts() ? 0
             : back ? (g_room_base[6] == id) + (g_room_base[7] == id) : (g_room_base[byte] == id ? 1 : 0);
    if (g_room_owned_known && g_room_owned[k] > have) have = g_room_owned[k];
    const int need = back && rec[byte == 6 ? 7 : 6] == id ? 2 : 1;
    return have >= need;
}

/* The garage screens (moa_menu_build.inc) ask about the AC being edited. */
static bool room_part_ok(int byte, int id) { return room_part_ok_in(g_edit.rec, byte, id); }

static bool room_option_ok(int number) {
    if (!room_own_parts_only()) return true;
    if (number < 0 || number >= MOA_OPTIONS) return false;
    const unsigned base_mask = g_room_base[10] | (unsigned)(g_room_base[11] & 0x0F) << 8;
    if (room_base_counts() && (base_mask & (1u << number))) return true;
    const int k = moa_owned_index(10, number);
    return k >= 0 && g_room_owned_known && g_room_owned[k] > 0;
}

/* The first part on `c` that isn't yours (FULL GARAGE off), or nullptr. */
static const char* room_unowned_part(const moa_card& c) {
    if (!room_own_parts_only()) return nullptr;
    static std::string name;
    const uint8_t* r = c.rec;
    for (int byte = 0; byte < 13; ++byte) {
        if (byte == 10 || byte == 11) continue;
        if ((byte == 4 || byte == 5) && r[2] >= 12 && r[2] != 255) continue;   /* weapon arms */
        if (byte == 12 && r[3] >= 27 && r[3] != 255) continue;                 /* caterpillar */
        if (room_part_ok_in(r, byte, r[byte])) continue;
        const moa_part* p = g_parts_state > 0 ? moa_parts_find(&g_parts, moa_record_cat(byte), r[byte]) : nullptr;
        name = p ? upper(p->name) : std::string("A PART");
        return name.c_str();
    }
    const unsigned mask = r[10] | (unsigned)(r[11] & 0x0F) << 8;
    for (int o = 0; o < MOA_OPTIONS; ++o) {
        if (!(mask & (1u << o)) || room_option_ok(o)) continue;
        name = g_parts_state > 0 ? upper(g_parts.option[o].name) : std::string("AN OPTIONAL PART");
        return name.c_str();
    }
    return nullptr;
}

static bool room_ac_differs(const moa_card& a, const moa_card& b) {
    return std::memcmp(a.rec, b.rec, sizeof a.rec) != 0 || a.plus != b.plus ||
           std::memcmp(a.paint, b.paint, sizeof a.paint) != 0 ||
           std::memcmp(a.palette, b.palette, sizeof a.palette) != 0 ||
           std::memcmp(a.emblem, b.emblem, sizeof a.emblem) != 0 ||
           std::strcmp(a.ac, b.ac) != 0 || std::strcmp(a.pilot, b.pilot) != 0;
}

/* Back from a full-garage screen (moa_menu_build.inc, leave_to_garage). */
static void room_take_edit() {
    if (!g_room_has_ac) {          /* built from nothing: it came with no parts */
        std::memset(g_room_base, 255, sizeof g_room_base);
        g_room_base[10] = g_room_base[11] = 0;
        g_room_base_plus = 0;
        g_room_ac_from = "GARAGE";
    }
    if (!g_room_has_ac || room_ac_differs(g_room_ac, g_edit)) {
        g_room_ac = g_edit;
        g_room_has_ac = true;
        g_room_ac_edited = true;
        room_ac_save();
    }
    g_room_view = RV_GARAGE;
}

/* ---- loading an AC ---- */

struct RoomSave { std::string label, where, parts_from; moa_card card; uint8_t owned[256]; int owned_n; };
static std::vector<RoomSave> g_room_saves;

/* Every Master of Arena save on the .mcd files in the memory card folder. */
static void room_scan_saves(LauncherModel* m) {
    namespace fs = std::filesystem;
    g_room_saves.clear();
    std::vector<std::string> files;
    const std::string folder = ai_card_folder(m);
    std::error_code ec;
    if (!folder.empty())
        for (fs::directory_iterator it(fs::u8path(folder), ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code fec;
            if (!it->is_regular_file(fec)) continue;
            if (upper(it->path().extension().u8string().c_str()) == ".MCD")
                files.push_back(it->path().u8string());
        }
    std::sort(files.begin(), files.end());
    const char* card1 = m->s.memcard_path[0];
    if (card1 && card1[0] && std::find(files.begin(), files.end(), card1) == files.end())
        files.insert(files.begin(), card1);
    const std::string mine = room_card_path();
    std::vector<uint8_t> img(131072);
    for (const std::string& path : files) {
        if (fs::u8path(path) == fs::u8path(mine)) continue;   /* the room's own match card */
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) continue;
        const size_t got = std::fread(img.data(), 1, img.size(), f);
        std::fclose(f);
        if (got != img.size() || img[0] != 'M' || img[1] != 'C') continue;
        const std::string stem = upper(fs::u8path(path).stem().u8string().c_str());
        for (int i = 1; i < 16; ++i) {
            const uint8_t* fr = img.data() + i * 128;
            if (fr[0] != 0x51) continue;                       /* first block of a save */
            if (std::strncmp((const char*)fr + 10, "BASLUS-0103", 11) != 0) continue;
            RoomSave s;
            if (!moa_card_from_save(img.data() + i * 8192, 8192, &s.card)) continue;
            if (!moa_save_owned(img.data() + i * 8192, 8192, s.owned)) std::memset(s.owned, 0, sizeof s.owned);
            s.owned_n = 0;
            for (int k = 0; k < 256; ++k) s.owned_n += s.owned[k] != 0;
            s.label = upper(s.card.ac[0] ? s.card.ac : "AC");
            char where[96];
            std::snprintf(where, sizeof where, "%s  BLOCK %d", stem.c_str(), i);
            s.parts_from = where;
            std::snprintf(where, sizeof where, "%s  BLOCK %d  ·  %d PARTS OWNED", stem.c_str(), i, s.owned_n);
            s.where = where;
            g_room_saves.push_back(s);
        }
    }
}

/* ---- checks ---- */

static void room_check(const moa_card& c, struct moa_build_check* out) {
    std::memset(out, 0, sizeof *out);
    if (g_parts_state > 0) moa_build_check(&g_parts, c.rec, c.plus, out);
}

/* Why this pilot can't be READY now, or nullptr. */
static const char* room_ready_problem(const LauncherModel* m, bool is_host) {
    if (!launcher_model_netplay_disc_ok(m)) return "The game disc is needed for network play.";
    if (!is_host && !g_room_heard) return "Waiting for the host's match settings.";
    if (!g_room_has_ac) return "Load an AC in the GARAGE first.";
    if (room_own_parts_only()) {
        if (const char* part = room_unowned_part(g_room_ac)) {
            static char b[128];
            std::snprintf(b, sizeof b, "FULL GARAGE is off: %s is not one of your parts.", part);
            return b;
        }
        int plus_have = room_base_counts() ? g_room_base_plus : 0;
        if (g_room_owned_known && g_room_owned_plus > plus_have) plus_have = g_room_owned_plus;
        if (g_room_ac.plus > plus_have) return "FULL GARAGE is off: your save has no HUMAN PLUS for this AC.";
    }
    if (parts_ready(m)) {
        struct moa_build_check chk;
        room_check(g_room_ac, &chk);
        if (chk.blocked) return "The game will not field this AC: fix the build.";
    }
    return nullptr;
}

/* READY: the match card and what the game reads at launch. False (and a
 * note) when the card could not be written. */
static bool room_arm(LauncherModel* m) {
    (void)m;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(room_card_path()).parent_path(), ec);
    const moa_card* units[1] = { &g_room_ac };
    if (!moa_memcard_write_units(room_card_path().c_str(), units, 1)) {
        note("THE MATCH CARD COULD NOT BE WRITTEN");
        return false;
    }
    /* No offline mode's choices in a match. */
    set_ai_battle(false);
    set_arena_run(false, "");
    set_env("PSX_MOA_START", "");
    set_env("PSX_MEMCARD1_NETPLAY", room_card_path().c_str());
    set_env("PSX_MOA_MODELS_DIR", models_installed() ? models_dir().c_str() : "");
    set_env("PSX_MOA_ROOM_RESULT", room_result_path().c_str());
    std::remove(room_result_path().c_str());
    return true;
}

/* Not READY any more: an offline game started later must not find the card. */
static void room_disarm() {
    set_env("PSX_MEMCARD1_NETPLAY", "");
}

/* Back from a match: the set's result (games/master-of-arena/moa_netplay_room.c
 * writes "p1=W;p2=W;rounds=N;done=1"). */
static bool room_read_result(const LobbySnapshot& s) {
    g_room_result.clear();
    FILE* f = std::fopen(room_result_path().c_str(), "rb");
    if (!f) return false;
    char line[128] = {0};
    const bool got = std::fgets(line, sizeof line, f) != nullptr;
    std::fclose(f);
    if (!got) return false;
    int p1 = -1, p2 = -1, done = 0;
    for (const char* p = line; p && *p;) {
        if (std::strncmp(p, "p1=", 3) == 0) p1 = std::atoi(p + 3);
        else if (std::strncmp(p, "p2=", 3) == 0) p2 = std::atoi(p + 3);
        else if (std::strncmp(p, "done=", 5) == 0) done = std::atoi(p + 5);
        const char* semi = std::strchr(p, ';');
        p = semi ? semi + 1 : nullptr;
    }
    if (p1 < 0 || p2 < 0) return false;
    std::string n1 = s.max_slots > 0 && s.occupied[0] ? upper(s.slots[0].display_name) : std::string("P1");
    std::string n2 = s.max_slots > 1 && s.occupied[1] ? upper(s.slots[1].display_name) : std::string("P2");
    if (n1.size() > 12) n1 = n1.substr(0, 12);
    if (n2.size() > 12) n2 = n2.substr(0, 12);
    char b[96];
    std::snprintf(b, sizeof b, "%s %d - %d %s%s", n1.c_str(), p1, p2, n2.c_str(),
                  done ? "" : "  (NOT FINISHED)");
    g_room_result = b;
    return done != 0;
}

/* ---- drawing helpers ---- */

static void draw_room_stage(const LauncherModel* m, const Gfx& g, float x, float y,
                            float w, float h, int id) {
    if (g_maps_state == 0) {
        g_maps = new moa_map_previews;
        const char* p = launcher_model_effective_rom_path(m);
        if (!p || !p[0]) p = launcher_model_rom_path(m);
        g_maps_state = (p && p[0] && moa_map_previews_load(p, g_maps)) ? 1 : -1;
    }
    if (id < MOA_STAGE_FIRST || id >= MOA_STAGE_FIRST + MOA_STAGES || g_maps_state < 0) {
        g.dl->AddRect(at(g, x, y), at(g, x + w, y + h), kLine);
        const char* label = id == 0 ? "?" : room_stage_name(id);
        const float size = id == 0 ? 20.0f : 11.0f;
        text(g, x + (w - text_width(g, size, label)) * 0.5f, y + h * 0.5f - size * 0.6f, size, kDim, label);
        return;
    }
    const uint32_t* px = g_maps->rgba[id - MOA_STAGE_FIRST];
    const float cw = w / 52.0f, ch = h / 50.0f;
    for (int yy = 0; yy < 50; ++yy)
        for (int xx = 0; xx < 52; ++xx) {
            const uint32_t c = px[(yy * 2) * MOA_MAP_W + xx * 2];
            g.dl->AddRectFilled(at(g, x + xx * cw, y + yy * ch), at(g, x + (xx + 1) * cw, y + (yy + 1) * ch),
                                IM_COL32(c & 255, (c >> 8) & 255, (c >> 16) & 255, 255));
        }
    g.dl->AddRect(at(g, x, y), at(g, x + w, y + h), kLine);
}

/* The AC on the right of the garage and load screens. */
static void draw_room_ac_box(const Gfx& g, float x, float y, float w, float h,
                             const moa_card* c, const char* from, bool yours = false) {
    panel(g, x, y, w, h, "YOUR AC");
    if (!c) {
        text(g, x + 14.0f, y + 20.0f, 11.0f, kDim, "NO AC YET. LOAD ONE FROM A MEMORY CARD OR AN AI CARD.", w - 28.0f);
        return;
    }
    draw_acv(g, x + 2.0f, y + 2.0f, w - 4.0f, h - 92.0f, *c);
    draw_emblem(g, x + 10.0f, y + 10.0f, 32.0f, *c);
    text_right(g, x + w - 10.0f, y + 10.0f, 12.0f, kBright, upper(c->ac[0] ? c->ac : "AC").c_str());
    text_right(g, x + w - 10.0f, y + 26.0f, 10.0f, kDim, upper(c->pilot).c_str());
    float ty = y + h - 84.0f;
    if (from && from[0]) {
        text(g, x + 12.0f, ty, 10.0f, kDim, "FROM");
        text(g, x + 70.0f, ty, 10.0f, kText, from);
        ty += 16.0f;
    }
    if (g_parts_state > 0) {
        struct moa_build_check chk;
        room_check(*c, &chk);
        char b[96];
        std::snprintf(b, sizeof b, "AP %d   WEIGHT %d", chk.ap, chk.weight);
        text(g, x + 12.0f, ty, 10.0f, kText, b);
        ty += 16.0f;
        const char* unowned = yours ? room_unowned_part(*c) : nullptr;
        char b2[96];
        if (unowned) std::snprintf(b2, sizeof b2, "NOT YOUR PART: %s", unowned);
        text(g, x + 12.0f, ty, 10.0f, chk.blocked || unowned ? kWarn : kOn,
             chk.blocked ? "THE GAME WILL NOT FIELD THIS BUILD" : unowned ? b2 : "READY TO FIELD", w - 24.0f);
        ty += 16.0f;
    }
    if (c->plus) text(g, x + 12.0f, ty, 10.0f, kHi, "HUMAN PLUS");
}

/* ---- GARAGE ---- */

static void room_open_builder(LauncherModel* m, int sub) {
    if (!parts_ready(m)) { note("THE PART LIST COULD NOT BE READ FROM THE DISC"); return; }
    if (room_own_parts_only() && !g_room_has_ac) { note("LOAD AN AC FIRST"); return; }
    if (room_own_parts_only() && !g_room_owned_known && (sub == AI_SUB_ASM || sub == AI_SUB_OPTS)) {
        note("LOAD YOUR SAVE FROM A MEMORY CARD FIRST: YOUR PARTS COME FROM IT");
        return;
    }
    AiCard c;
    c.card = g_room_ac;
    c.path = room_ac_path();
    open_build(c);
    g_edit_dirty = false;
    g_room_garage = true;
    g_room_view = RV_BUILDER;
    if (sub == AI_SUB_ASM) {
        g_build_standalone = true;
        g_from_garage = false;
        open_asm(0, false);
        return;
    }
    g_build_standalone = false;
    g_from_garage = true;
    g_ai_sub = sub;
    if (sub == AI_SUB_OPTS) g_state.cursor[PAGE_AI_OPTS] = 0;
    if (sub == AI_SUB_PAINT) {
        g_split_paint = !paint_uniform();
        g_paint_section = 0;
        g_state.cursor[PAGE_AI_PAINT] = 0; g_state.top[PAGE_AI_PAINT] = 0;
    }
    if (sub == AI_SUB_EMBLEM) {
        g_state.cursor[PAGE_AI_EMBLEM] = 0; g_state.top[PAGE_AI_EMBLEM] = 0;
        g_emblem_files.clear();
    }
}

enum RoomGarageRow { RG_SAVE = 0, RG_CARD, RG_EXTRA, RG_ASM, RG_OPTS, RG_PAINT, RG_EMBLEM, RG_BACK };

static void draw_room_garage(LauncherModel* m, const Gfx& g, const Input& in) {
    room_ac_read();
    (void)parts_ready(m);
    const bool full = g_room.garage;
    RoomGarageRow rows[8];
    int count = 0;
    rows[count++] = RG_SAVE;
    rows[count++] = RG_CARD;
    if (full) rows[count++] = RG_EXTRA;
    rows[count++] = RG_ASM;
    rows[count++] = RG_OPTS;
    rows[count++] = RG_PAINT;
    rows[count++] = RG_EMBLEM;
    rows[count++] = RG_BACK;
    move_cursor(PAGE_LOBBY, count, in, count);
    int& cur = g_state.cursor[PAGE_LOBBY];

    header(g, "GARAGE", full ? "VERSUS ROOM  ·  FULL GARAGE" : "VERSUS ROOM");
    panel(g, 24.0f, 76.0f, 270.0f, 340.0f, full ? "FULL GARAGE" : "YOUR PARTS ONLY");
    static const char* const labels[] = { "LOAD FROM MEMORY CARD", "LOAD AI CARD", "LOAD EXTRA GARAGE AC",
                                          "ASSEMBLY", "OPTIONAL PARTS", "PAINT", "EMBLEM", "BACK" };
    int clicked = -1;
    for (int i = 0; i < count; ++i) {
        bool hovered = false;
        const bool sub = rows[i] != RG_EXTRA && rows[i] != RG_BACK;
        if (row(g, 25.0f, 90.0f + kRowH * i, 268.0f, i == cur, true, labels[rows[i]],
                sub ? ">" : nullptr, false, false, &hovered))
            clicked = i;
        if (hover_moved(hovered)) cur = i;
    }
    static const char* const info_full[] = {
        "Bring the AC of a Master of Arena save on one of your memory cards.",
        "Bring the AC of an AI card from your memory card folder.",
        "Bring the AC kept in EXTRA GARAGE.",
        "Change parts, with every part unlocked.",
        "Fit optional parts and set HUMAN PLUS.",
        "Change the AC's colours.",
        "Change the AC's emblem.",
        "Back to the room.",
    };
    static const char* const info_own[] = {
        "Bring the AC of a Master of Arena save on one of your memory cards, and that save's parts.",
        "Bring the AC of an AI card from your memory card folder. Its parts come with it.",
        "",
        "Change parts, with the parts you own.",
        "Fit the optional parts you own.",
        "Change the AC's colours.",
        "Change the AC's emblem.",
        "Back to the room.",
    };
    text(g, 38.0f, 300.0f, 10.0f, kText, (full ? info_full : info_own)[rows[cur]], 244.0f);
    if (!full) {
        char b[160];
        if (g_room_owned_known)
            std::snprintf(b, sizeof b, "FULL GARAGE is off. YOUR PARTS: %d, from %s.", g_room_owned_count,
                          g_room_owned_from.c_str());
        else
            std::snprintf(b, sizeof b, "FULL GARAGE is off. Load your save from a memory card to bring your parts.");
        text(g, 38.0f, 352.0f, 10.0f, kDim, b, 244.0f);
    }

    draw_room_ac_box(g, 310.0f, 76.0f, 306.0f, 340.0f, g_room_has_ac ? &g_room_ac : nullptr,
                     g_room_ac_from.c_str(), true);
    footer(g, "A / ENTER  SELECT      B / ESC  BACK");

    int activate = -1;
    if (clicked >= 0) activate = cur = clicked;
    else if (in.confirm) activate = cur;
    else if (in.back) { g_room_view = RV_MAIN; g_state.cursor[PAGE_LOBBY] = 1; return; }
    if (activate < 0) return;
    switch (rows[activate]) {
        case RG_SAVE:
            room_scan_saves(m);
            g_state.cursor[PAGE_AI_PICK] = 0; g_state.top[PAGE_AI_PICK] = 0;
            g_room_view = RV_SAVES;
            break;
        case RG_CARD:
            scan_ai_cards(m);
            g_state.cursor[PAGE_AI_PICK] = 0; g_state.top[PAGE_AI_PICK] = 0;
            g_room_view = RV_CARDS;
            break;
        case RG_EXTRA:
            load_garage();
            room_ac_set(g_garage, "EXTRA GARAGE");
            note("EXTRA GARAGE AC LOADED");
            break;
        case RG_ASM:    room_open_builder(m, AI_SUB_ASM); break;
        case RG_OPTS:   room_open_builder(m, AI_SUB_OPTS); break;
        case RG_PAINT:  room_open_builder(m, AI_SUB_PAINT); break;
        case RG_EMBLEM: room_open_builder(m, AI_SUB_EMBLEM); break;
        default:        g_room_view = RV_MAIN; g_state.cursor[PAGE_LOBBY] = 1; break;
    }
}

/* LOAD FROM MEMORY CARD / LOAD AI CARD: a list, the highlighted AC on the right. */
static void draw_room_load(LauncherModel* m, const Gfx& g, const Input& in, bool saves) {
    (void)parts_ready(m);
    const int n = saves ? (int)g_room_saves.size() : (int)g_ai_cards.size();
    const int visible = 10;
    move_cursor(PAGE_AI_PICK, n, in, visible);
    int& cur = g_state.cursor[PAGE_AI_PICK];
    const int top = g_state.top[PAGE_AI_PICK];

    header(g, saves ? "LOAD FROM MEMORY CARD" : "LOAD AI CARD", "VERSUS ROOM  ·  GARAGE");
    panel(g, 24.0f, 76.0f, 270.0f, 340.0f, saves ? "SAVES" : "AI CARDS");
    int clicked = -1;
    for (int i = top; i < n && i < top + visible; ++i) {
        bool hovered = false;
        const std::string label = saves ? g_room_saves[(size_t)i].label : g_ai_cards[(size_t)i].label;
        if (row(g, 25.0f, 86.0f + kRowH * (i - top), 268.0f, i == cur, true, label.c_str(), nullptr,
                false, false, &hovered))
            clicked = i;
        if (hover_moved(hovered)) cur = i;
    }
    if (n == 0)
        text(g, 40.0f, 96.0f, 11.0f, kDim,
             saves ? "NO MASTER OF ARENA SAVES ON YOUR MEMORY CARDS"
                   : "NO AI CARDS IN YOUR MEMORY CARD FOLDER", 240.0f);
    if (n > top + visible) text(g, 55.0f, 352.0f, 9.0f, kDim, "MORE BELOW");
    if (top > 0) text_right(g, 280.0f, 352.0f, 9.0f, kDim, "MORE ABOVE");
    if (saves && n > 0 && cur < n)
        text(g, 38.0f, 372.0f, 10.0f, kDim, g_room_saves[(size_t)cur].where.c_str(), 244.0f);

    const moa_card* c = nullptr;
    if (n > 0 && cur < n) c = saves ? &g_room_saves[(size_t)cur].card : &g_ai_cards[(size_t)cur].card;
    draw_room_ac_box(g, 310.0f, 76.0f, 306.0f, 340.0f, c, nullptr);
    footer(g, "A / ENTER  BRING THIS AC      B / ESC  BACK");

    if (in.back) { g_room_view = RV_GARAGE; return; }
    if (clicked >= 0) cur = clicked;
    if ((in.confirm || clicked >= 0) && c) {
        if (saves) {      /* the save's parts are your parts from now on */
            const RoomSave& sv = g_room_saves[(size_t)cur];
            std::memcpy(g_room_owned, sv.owned, sizeof g_room_owned);
            g_room_owned_known = true;
            g_room_owned_count = sv.owned_n;
            g_room_owned_from = sv.parts_from;
            g_room_owned_plus = sv.card.plus;
        }
        room_ac_set(*c, saves ? "MEMORY CARD" : "AI CARD");
        note(saves ? "AC AND PARTS LOADED" : "AC LOADED");
        g_room_view = RV_GARAGE;
    }
}

/* ---- MATCH SETTINGS ---- */

enum RoomSetRow { RS_STAGE = 0, RS_TIME, RS_ROUNDS, RS_GARAGE, RS_LOADING, RS_BACK, RS__COUNT };

static int room_stage_index(int id) {
    for (int i = 0; i < kRoomStageCount; ++i)
        if (kRoomStages[i].id == id) return i;
    return 0;
}

static void draw_room_settings(LauncherModel* m, const Gfx& g, const Input& in, bool is_host) {
    const bool editable = is_host && !g_room_ready;
    move_cursor(PAGE_NET_MODE, RS__COUNT, in, RS__COUNT);
    int& cur = g_state.cursor[PAGE_NET_MODE];

    header(g, "MATCH SETTINGS", is_host ? "VERSUS ROOM  ·  HOST" : "VERSUS ROOM  ·  SET BY THE HOST");
    panel(g, 24.0f, 76.0f, 340.0f, 214.0f, "MATCH");
    static const char* const labels[RS__COUNT] = { "STAGE", "TIME LIMIT", "ROUNDS", "FULL GARAGE",
                                                   "LOADING", "BACK" };
    int clicked = -1;
    for (int i = 0; i < RS__COUNT; ++i) {
        bool hovered = false;
        std::string value;
        bool toggle = false, on = false;
        switch (i) {
            case RS_STAGE:   value = std::string("< ") + room_stage_name(g_room.stage) + " >"; break;
            case RS_TIME:    value = std::string("< ") + kRoomTimes[g_room.time] + " >"; break;
            case RS_ROUNDS:  value = std::string("< ") + room_rounds_label(g_room.rounds) + " >"; break;
            case RS_GARAGE:  toggle = true; on = g_room.garage; break;
            case RS_LOADING: value = g_room.fast ? "< FAST >" : "< ORIGINAL >"; break;
            default: break;
        }
        const bool enabled = i == RS_BACK || editable;
        if (row(g, 25.0f, 90.0f + kRowH * i, 338.0f, i == cur, enabled, labels[i],
                value.empty() ? nullptr : value.c_str(), toggle, on, &hovered))
            clicked = i;
        if (hover_moved(hovered)) cur = i;
    }

    panel(g, 380.0f, 76.0f, 236.0f, 214.0f, "STAGE");
    draw_room_stage(m, g, 394.0f, 92.0f, 208.0f, 172.0f, g_room.stage);
    text(g, 394.0f, 270.0f, 10.0f, kBright, room_stage_name(g_room.stage));

    static const char* const info[RS__COUNT] = {
        "Where the match is fought. RANDOM picks one when the match starts.",
        "The game's own VS TIME LIMIT. When time runs out, the AC with more AP wins.",
        "SINGLE: one fight. BEST OF 3 / 5: the first pilot to win 2 / 3 fights takes the set.",
        "OFF: each pilot builds with the parts their own save owns (or brings an AI card's AC as it is). "
        "ON: every part is unlocked, and the EXTRA GARAGE AC can be brought.",
        "FAST: both PCs read the disc at the fast speed, from the start of the match to the end of the set. "
        "ORIGINAL: the drive's real speed.",
        "Back to the room.",
    };
    panel(g, 24.0f, 306.0f, 592.0f, 110.0f, "INFO");
    text(g, 38.0f, 322.0f, 11.0f, kText, info[cur], 564.0f);
    if (is_host && g_room_ready)
        text(g, 38.0f, 390.0f, 10.0f, kHi, "Leave READY to change the settings.", 564.0f);
    else if (!is_host)
        text(g, 38.0f, 390.0f, 10.0f, kDim, "The host sets these. You see them as they change.", 564.0f);
    footer(g, editable ? "LEFT / RIGHT  CHANGE      A / ENTER  SELECT      B / ESC  BACK"
                       : "B / ESC  BACK");

    if (in.back) { g_room_view = RV_MAIN; return; }
    if (clicked >= 0) cur = clicked;
    const bool confirm = in.confirm || clicked >= 0;
    if (cur == RS_BACK) { if (confirm) g_room_view = RV_MAIN; return; }
    if (!editable) return;
    const int dir = in.left ? -1 : (in.right || confirm) ? 1 : 0;
    if (!dir) return;
    switch (cur) {
        case RS_STAGE: {
            const int i = (room_stage_index(g_room.stage) + dir + kRoomStageCount) % kRoomStageCount;
            g_room.stage = kRoomStages[i].id;
            break;
        }
        case RS_TIME:    g_room.time = (g_room.time + dir + 3) % 3; break;
        case RS_ROUNDS: {
            int i = g_room.rounds == 5 ? 2 : g_room.rounds == 3 ? 1 : 0;
            i = (i + dir + 3) % 3;
            g_room.rounds = kRoomRounds[i];
            break;
        }
        case RS_GARAGE:  g_room.garage = !g_room.garage; break;
        case RS_LOADING: g_room.fast = !g_room.fast; break;
        default: break;
    }
}

/* ---- the room itself ---- */

enum LobbyRow { LOBBY_READY = 0, LOBBY_GARAGE, LOBBY_SETTINGS, LOBBY_CONNECTION, LOBBY_LEAVE };

static void draw_pilot(const Gfx& g, float x, float y, const LobbySnapshot& s, int seat,
                       bool remote_ready) {
    const float w = 288.0f, h = 148.0f;
    char buf[96];
    std::snprintf(buf, sizeof buf, "P%d", seat + 1);
    panel(g, x, y, w, h, buf);
    if (seat >= s.max_slots || !s.occupied[seat]) {
        text(g, x + 14.0f, y + 20.0f, 13.0f, kOff, "- OPEN -");
        return;
    }
    const RecompLauncherCNetplayMember& mem = s.slots[seat];
    std::string name = upper(mem.display_name);
    if (name.size() > 16) name = name.substr(0, 15) + ".";
    if (mem.is_local)
        g.dl->AddRectFilledMultiColor(at(g, x + 1.0f, y + 1.0f), at(g, x + w - 1.0f, y + 30.0f),
                                      IM_COL32(95, 224, 138, 40), IM_COL32(95, 224, 138, 0),
                                      IM_COL32(95, 224, 138, 0), IM_COL32(95, 224, 138, 40));
    text(g, x + 14.0f, y + 12.0f, 13.0f, mem.is_local ? kBright : kText, name.c_str());
    if (mem.is_host) lamp(g, x + w - 12.0f, y + 10.0f, true, "HOST");
    else if (mem.latency_ms >= 0 && !mem.is_local) {
        std::snprintf(buf, sizeof buf, "%d MS", mem.latency_ms);
        text_right(g, x + w - 12.0f, y + 13.0f, 10.0f, kDim, buf);
    }
    const float ey = y + 40.0f;
    bool ready = remote_ready;
    if (mem.is_local) {
        ready = g_room_ready;
        if (g_room_has_ac) {
            draw_emblem(g, x + 14.0f, ey, 56.0f, g_room_ac);
            text(g, x + 84.0f, ey + 2.0f, 12.0f, kBright, upper(g_room_ac.ac[0] ? g_room_ac.ac : "AC").c_str());
            text(g, x + 84.0f, ey + 20.0f, 10.0f, kDim, upper(g_room_ac.pilot).c_str());
            if (g_parts_state > 0) {
                struct moa_build_check chk;
                room_check(g_room_ac, &chk);
                std::snprintf(buf, sizeof buf, "AP %d   WEIGHT %d", chk.ap, chk.weight);
                text(g, x + 84.0f, ey + 38.0f, 10.0f, kText, buf);
            }
        } else {
            g.dl->AddRect(at(g, x + 14.0f, ey), at(g, x + 70.0f, ey + 56.0f), kLine);
            text(g, x + 84.0f, ey + 2.0f, 11.0f, kDim, "NO AC YET", w - 98.0f);
        }
    } else {
        g.dl->AddRect(at(g, x + 14.0f, ey), at(g, x + 70.0f, ey + 56.0f), kLine);
        text(g, x + 36.0f, ey + 14.0f, 22.0f, kDim, "?");
        text(g, x + 84.0f, ey + 2.0f, 11.0f, kDim, "AC SHOWN AT LAUNCH", w - 98.0f);
    }
    const ImU32 col = ready ? kOn : kOff;
    const char* what = ready ? "READY" : "NOT READY";
    g.dl->AddRectFilled(at(g, x + 14.0f, y + h - 24.0f), at(g, x + 23.0f, y + h - 15.0f), col);
    text(g, x + 32.0f, y + h - 27.0f, 12.0f, col, what);
}

static void draw_lobby_page(LauncherModel* m, const LauncherTheme& th,
                            const Gfx& g, const Input& in) {
    const auto* np = np_cb(m);
    if (!np) { launcher_model_set_view(m, LNG_VIEW_DASHBOARD); return; }
    /* The stock room page's upkeep, in its order. It runs whatever the room
     * shows (the garage and its screens included), so the seat never lapses. */
    if (np->pump) np->pump(np->ctx);
    np_ingest_last_error(m, np);
    if (np->local_launch_gate_set)
        np->local_launch_gate_set(np->ctx, launcher_model_can_launch(m) ? 1 : 0);
    if (np->launch_pending && np->launch_pending(np->ctx)) {
        if (!g_room_launched) g_room_launch_at = std::chrono::steady_clock::now();
        g_room_launched = true;
        np_try_launch(m);
    }
    if (!np_lobby_seated(m, np)) {
        header(g, "VERSUS ROOM", "LINK SYSTEM");
        text(g, 38.0f, 92.0f, 12.0f, kDim, "LEAVING THE ROOM");
        if (!g_room_launched) {
            g_room_ready = false;
            g_room_heard = false;
            g_room_view = RV_MAIN;
            g_room_garage = false;
        }
        return;
    }
    /* Two seats, nothing to trade: an ask to swap is answered no. */
    if (np->seat_swap_incoming && np->seat_swap_respond) {
        char who[64] = {0};
        int from_slot = -1;
        if (np->seat_swap_incoming(np->ctx, who, sizeof who, &from_slot))
            (void)np->seat_swap_respond(np->ctx, 0);
    }

    LobbySnapshot s;
    np_lobby_snapshot(m, np, &s);
    room_ac_read();

    /* Back from a match: everyone readies again. The launcher is not drawn
     * while a match runs, so a long gap between two frames of this page means
     * one ran (a launch that never happened leaves no gap). */
    static std::chrono::steady_clock::time_point last_draw;
    const auto now_clock = std::chrono::steady_clock::now();
    const bool gap = last_draw.time_since_epoch().count() != 0 &&
                     now_clock - last_draw > std::chrono::seconds(3);
    last_draw = now_clock;
    /* A launch that never left the launcher (refused, or START failed). */
    if (g_room_launched && !gap && now_clock - g_room_launch_at > std::chrono::seconds(10))
        g_room_launched = false;
    if (g_room_launched && gap) {
        g_room_launched = false;
        g_room_ready = false;
        g_room_count_at = 0.0;
        g_room_started = false;
        g_room_view = RV_MAIN;
        room_disarm();
        if (room_read_result(s)) {
            /* The set ended as planned: the session's "the other player left"
             * is just the peer ending it a moment earlier. */
            m->netplay_status[0] = '\0';
            if (np->clear_last_error) np->clear_last_error(np->ctx);
        }
    }

    int local_slot = -1, remote_slot = -1;
    for (int i = 0; i < s.max_slots; ++i) {
        if (!s.occupied[i] || s.slots[i].is_spectator) continue;
        if (s.slots[i].is_local) local_slot = i;
        else if (remote_slot < 0) remote_slot = i;
    }

    /* The match options: the host publishes its own, a guest reads the host's. */
    /* From START on, the options the host settled stand (the game reads them
     * as this session starts): nothing here may touch them. */
    const bool launching = g_room_launched;
    if (s.is_host) {
        g_room_heard = true;
        if (np->room_opts_set && !launching)
            (void)np->room_opts_set(np->ctx, room_opts_text(g_room, g_room.stage).c_str());
        /* The guest's card is the match's slot 2: always allowed here. */
        if (np->guest_memcard_get && np->guest_memcard_set && !np->guest_memcard_get(np->ctx))
            (void)np->guest_memcard_set(np->ctx, 1);
    } else {
        char line[128] = {0};
        RoomOpts heard;
        if (np->room_opts_get && np->room_opts_get(np->ctx, line, sizeof line) &&
            room_opts_parse(line, &heard)) {
            g_room = heard;
            g_room_heard = true;
        } else if (!launching) {
            g_room_heard = false;
        }
        if (g_room_ready && !launching && !room_opts_same(g_room_ready_opts, g_room)) {
            g_room_ready = false;
            room_disarm();
            note("THE HOST CHANGED THE MATCH SETTINGS: READY AGAIN");
        }
    }
    /* A READY that no longer holds (the AC or the rules changed). */
    if (g_room_ready && !launching && room_ready_problem(m, s.is_host)) {
        g_room_ready = false;
        room_disarm();
    }
    /* READY rides on the card offer: has_card always (the room's match card),
     * share = READY. The seat table shows it to the other pilot. */
    if (np->memcard_offer_set && !launching)
        (void)np->memcard_offer_set(np->ctx, 1, g_room_ready ? 1 : 0);

    const bool remote_ready = remote_slot >= 0 && s.slots[remote_slot].memcard_offer_valid &&
                              s.slots[remote_slot].memcard_has_card && s.slots[remote_slot].memcard_share;
    const bool room_ok = s.seated_players >= 2 && !s.local_rom_missing &&
                         s.peers_missing_files == 0 && s.local_missing_files == 0;
    const bool both = g_room_ready && remote_ready && room_ok;
    const double now = ImGui::GetTime();
    if (!both) {
        g_room_count_at = 0.0;
        g_room_started = false;
    } else if (g_room_count_at <= 0.0) {
        g_room_count_at = now;
    }
    const int count_left = both ? 3 - (int)(now - g_room_count_at) : 0;
    if (both && s.is_host && !g_room_started && !launching && now - g_room_count_at >= 3.0) {
        /* RANDOM becomes one stage now, the same on both PCs. */
        int stage = g_room.stage;
        if (stage == 0) {
            static bool seeded = false;
            if (!seeded) { std::srand((unsigned)std::time(nullptr)); seeded = true; }
            stage = kRoomStages[std::rand() % (kRoomStageCount - 1)].id;
        }
        if (np->room_opts_set) (void)np->room_opts_set(np->ctx, room_opts_text(g_room, stage).c_str());
        g_room_started = true;
        g_room_launched = true;
        g_room_launch_at = std::chrono::steady_clock::now();
        np_lobby_start(m, np);
    }

    /* The garage's full screens draw in place of the room. */
    if (g_room_view == RV_BUILDER) {
        if (!g_room_garage || g_ai_sub == AI_SUB_SETUP || g_ai_sub == AI_SUB_CARDS) {
            g_room_garage = false;
            g_room_view = RV_GARAGE;
        } else {
            draw_ai_page(m, g, in);
            return;
        }
    }
    if (g_room_view == RV_GARAGE) { draw_room_garage(m, g, in); return; }
    if (g_room_view == RV_SAVES)  { draw_room_load(m, g, in, true); return; }
    if (g_room_view == RV_CARDS)  { draw_room_load(m, g, in, false); return; }
    if (g_room_view == RV_SETTINGS) { draw_room_settings(m, g, in, s.is_host); return; }

    const char* problem = room_ready_problem(m, s.is_host);
    struct Entry { LobbyRow id; const char* label; };
    const Entry entries[5] = {
        { LOBBY_READY, "READY" },
        { LOBBY_GARAGE, "GARAGE" },
        { LOBBY_SETTINGS, "MATCH SETTINGS" },
        { LOBBY_CONNECTION, s.is_host ? "CONNECTION" : "CONNECTION INFO" },
        { LOBBY_LEAVE, "LEAVE ROOM" },
    };
    const int count = 5;
    move_cursor(PAGE_LOBBY, count, in, count);
    int& cur = g_state.cursor[PAGE_LOBBY];

    header(g, "VERSUS ROOM", s.is_host ? "YOU ARE THE HOST" : "YOU ARE THE GUEST");
    draw_pilot(g, 24.0f, 66.0f, s, 0, local_slot != 0 && remote_ready);
    draw_pilot(g, 328.0f, 66.0f, s, 1, local_slot != 1 && remote_ready);

    panel(g, 24.0f, 228.0f, 288.0f, 188.0f, "COMMAND");
    int clicked = -1;
    for (int i = 0; i < count; ++i) {
        bool hovered = false;
        const bool is_ready = entries[i].id == LOBBY_READY;
        const bool enabled = !is_ready || g_room_ready || !problem;
        const bool locked = entries[i].id == LOBBY_GARAGE && g_room_ready;
        if (row(g, 25.0f, 240.0f + kRowH * i, 286.0f, i == cur, enabled && !locked, entries[i].label,
                is_ready ? nullptr : ">", is_ready, g_room_ready, &hovered))
            clicked = i;
        if (hover_moved(hovered)) cur = i;
    }
    if (problem && !g_room_ready && cur == 0)
        text(g, 38.0f, 376.0f, 10.0f, kWarn, problem, 262.0f);
    else if (g_room_ready && cur == 1)
        text(g, 38.0f, 376.0f, 10.0f, kDim, "Leave READY to change your AC.", 262.0f);

    panel(g, 328.0f, 228.0f, 288.0f, 188.0f, s.is_host ? "MATCH" : "MATCH  ·  SET BY HOST");
    {
        draw_room_stage(m, g, 340.0f, 240.0f, 104.0f, 86.0f, g_room_heard ? g_room.stage : 0);
        float y = 240.0f;
        const float lx = 456.0f;
        if (!g_room_heard) {
            text(g, lx, y, 10.0f, kDim, "WAITING FOR THE HOST", 150.0f);
        } else {
            text(g, lx, y, 10.0f, kDim, "STAGE");
            text(g, lx, y + 13.0f, 11.0f, kBright, room_stage_name(g_room.stage));
            text(g, lx, y + 32.0f, 10.0f, kDim, "TIME LIMIT");
            text(g, lx, y + 45.0f, 11.0f, kBright, kRoomTimes[g_room.time]);
            text(g, lx, y + 64.0f, 10.0f, kDim, "ROUNDS");
            text(g, lx, y + 77.0f, 11.0f, kBright, room_rounds_label(g_room.rounds));
            text(g, 340.0f, y + 96.0f, 10.0f, kDim, "GARAGE");
            text(g, 400.0f, y + 96.0f, 10.0f, g_room.garage ? kHi : kText,
                 g_room.garage ? "FULL (EVERY PART)" : "YOUR OWN PARTS");
        }
        y = 352.0f;
        if (s.seated_players < 2) {
            text(g, 340.0f, y, 11.0f, kHi, "STANDING BY");
            text(g, 340.0f, y + 16.0f, 10.0f, kText, "Waiting for another pilot to join.", 264.0f);
        } else if (s.local_rom_missing) {
            text(g, 340.0f, y, 10.0f, kWarn, "The game disc was not found on this machine.", 264.0f);
        } else if (both) {
            char b[48];
            std::snprintf(b, sizeof b, "LAUNCHING IN %d", count_left > 0 ? count_left : 0);
            text(g, 340.0f, y, 13.0f, kOn, b);
            text(g, 340.0f, y + 18.0f, 10.0f, kText, "Leave READY to stop.", 264.0f);
        } else if (g_room_ready) {
            text(g, 340.0f, y, 11.0f, kHi, "READY");
            text(g, 340.0f, y + 16.0f, 10.0f, kText, "Waiting for the other pilot.", 264.0f);
        } else {
            text(g, 340.0f, y, 11.0f, kHi, remote_ready ? "THE OTHER PILOT IS READY" : "STANDING BY");
            text(g, 340.0f, y + 16.0f, 10.0f, kText, "Set your AC in the GARAGE, then press READY.", 264.0f);
        }
        if (!g_room_result.empty())
            text(g, 340.0f, 398.0f, 10.0f, kDim, (std::string("LAST SET  ") + g_room_result).c_str(), 264.0f);
    }
    if (m->netplay_status[0] && !both)
        text(g, 38.0f, 396.0f, 10.0f, kWarn, m->netplay_status, 262.0f);
    if (s.is_host && s.seated_players < 2 && m->netplay_local_room && m->netplay_host_endpoint[0]) {
        char b[96];
        std::snprintf(b, sizeof b, "ADDRESS  %s", m->netplay_host_endpoint);
        text(g, 340.0f, 384.0f, 10.0f, kDim, b);
    }
    footer(g, "A / ENTER  SELECT      START  READY      B / ESC  LEAVE");

    moa_menu_lobby_popups(m, th, np, s);

    int activate = -1;
    if (clicked >= 0) activate = cur = clicked;
    else if (in.confirm) activate = cur;
    else if (in.start) activate = 0;
    else if (in.back && entries[cur].id != LOBBY_LEAVE) {
        /* Back does not drop a seat by accident: it goes to LEAVE ROOM, and
         * a second press there leaves. */
        cur = count - 1;
        note("PRESS AGAIN TO LEAVE THE ROOM");
    } else if (in.back) {
        activate = cur;
    }
    if (activate < 0) return;
    switch (entries[activate].id) {
        case LOBBY_READY:
            if (launching) break;
            if (g_room_ready) {
                g_room_ready = false;
                room_disarm();
            } else if (problem) {
                note("NOT READY YET");
            } else if (room_arm(m)) {
                g_room_ready = true;
                g_room_ready_opts = g_room;
            }
            break;
        case LOBBY_GARAGE:
            if (g_room_ready) { note("LEAVE READY TO CHANGE YOUR AC"); break; }
            g_state.cursor[PAGE_LOBBY] = 0;
            g_room_view = RV_GARAGE;
            break;
        case LOBBY_SETTINGS:
            g_state.cursor[PAGE_NET_MODE] = 0;
            g_room_view = RV_SETTINGS;
            break;
        case LOBBY_CONNECTION:
            g_lobby_settings_synced = false;
            m->netplay_lobby_settings_open = true;
            break;
        case LOBBY_LEAVE:
            g_state.cursor[PAGE_LOBBY] = 0;
            g_room_ready = false;
            g_room_launched = false;
            room_disarm();
            np_lobby_leave(m, np);
            break;
    }
}
