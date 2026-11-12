/* Master of Arena system menu: the network pages.
 *
 * Included by moa_menu.inc, inside namespace moa_menu. It owns the launcher's
 * three netplay views:
 *
 *   LNG_VIEW_NETPLAY_MODE   LAN or online, and the callsign
 *   LNG_VIEW_NETPLAY        the room list: host, join, join by address
 *   LNG_VIEW_LOBBY          the room: pilots, start, leave
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

enum ModeRow { MODE_LAN = 0, MODE_ONLINE, MODE_CALLSIGN, MODE_BACK, MODE__COUNT };

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

    header(g, "NETWORK PLAY", "LINK SYSTEM");
    panel(g, 24.0f, 76.0f, 270.0f, 340.0f, "SELECT");
    const std::string callsign = "< " + upper(np_effective_name(m)) + " >";
    const char* labels[MODE__COUNT] = { "LAN PLAY", "ONLINE PLAY", "CALLSIGN", "BACK" };
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
        if (offered) {
            text(g, 324.0f, y, 11.0f, kDim, "ACCOUNT");
            text(g, 424.0f, y, 11.0f, signed_in ? kOn : kText,
                 signed_in ? "SIGNED IN" : awaiting ? "CHECKING" : "GUEST");
        }
    }

    static const char* const info[MODE__COUNT] = {
        "Play someone on the same network, or connect straight to an address "
        "they give you. Rooms on your network appear by themselves.",
        "Find other pilots on the lobby server.",
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
                 : "Join as player 2. Your own memory card comes with you.",
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

/* ---- the room ---------------------------------------------------------------------- */

enum LobbyRow { LOBBY_START = 0, LOBBY_CARD, LOBBY_SETTINGS, LOBBY_MODS, LOBBY_LEAVE };

static void draw_lobby_page(LauncherModel* m, const LauncherTheme& th,
                            const Gfx& g, const Input& in) {
    const auto* np = np_cb(m);
    if (!np) { launcher_model_set_view(m, LNG_VIEW_DASHBOARD); return; }
    /* The stock room page's upkeep, in its order. */
    if (np->pump) np->pump(np->ctx);
    np_ingest_last_error(m, np);
    const int has_card = np_local_memcard_has_card(m);
    if (np->memcard_offer_set && has_card >= 0)
        (void)np->memcard_offer_set(np->ctx, has_card, -1);
    if (np->local_launch_gate_set)
        np->local_launch_gate_set(np->ctx, launcher_model_can_launch(m) ? 1 : 0);
    if (np->launch_pending && np->launch_pending(np->ctx))
        np_try_launch(m);
    if (!np_lobby_seated(m, np)) {
        header(g, "VERSUS ROOM", "LINK SYSTEM");
        text(g, 38.0f, 92.0f, 12.0f, kDim, "LEAVING THE ROOM");
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

    int local_slot = -1;
    for (int i = 0; i < s.max_slots; ++i)
        if (s.occupied[i] && s.slots[i].is_local) local_slot = i;

    /* The guest's own memory card: the guest offers it, the host allows it. */
    const bool card_known = np->memcard_offer_set && has_card >= 0 &&
                            s.max_slots > 1 && s.occupied[1] &&
                            s.slots[1].memcard_offer_valid;
    const bool card_allowed = !np->guest_memcard_get || np->guest_memcard_get(np->ctx) != 0;
    const bool card_offered = card_known && s.slots[1].memcard_has_card != 0;
    const bool card_shared = card_offered && s.slots[1].memcard_share != 0;
    const bool card_row = card_known && (s.is_host ? np->guest_memcard_set != nullptr
                                                   : local_slot == 1 && card_offered);

    const bool can_start = s.seated_players >= 2 && !s.local_rom_missing &&
                           s.peers_not_ready == 0 && s.peers_missing_files == 0 &&
                           s.local_missing_files == 0;
    const char* waiting =
        s.seated_players < 2 ? "Waiting for another pilot to join."
        : s.local_rom_missing ? "The game disc was not found on this machine."
        : s.local_missing_files > 0 ? "This room's mods need files you have not selected."
        : s.peers_not_ready > 0 ? "Waiting for the other pilot to be ready."
        : s.peers_missing_files > 0 ? "The other pilot is missing files this room's mods need."
        : nullptr;

    struct Entry { LobbyRow id; const char* label; bool toggle; bool on; };
    Entry entries[5];
    int count = 0;
    if (s.is_host)
        entries[count++] = { LOBBY_START, "START MATCH", false, false };
    if (card_row)
        entries[count++] = { LOBBY_CARD, s.is_host ? "GUEST MEMORY CARD"
                                                   : "BRING MY MEMORY CARD",
                             true, s.is_host ? card_allowed : card_shared };
    entries[count++] = { LOBBY_SETTINGS, s.is_host ? "ROOM SETTINGS" : "ROOM INFO",
                         false, false };
    if (m->mods)
        entries[count++] = { LOBBY_MODS, s.is_host ? "ROOM MODS" : "VIEW ROOM MODS",
                             false, false };
    entries[count++] = { LOBBY_LEAVE, "LEAVE ROOM", false, false };

    move_cursor(PAGE_LOBBY, count, in, count);
    int& cur = g_state.cursor[PAGE_LOBBY];

    header(g, "VERSUS ROOM", s.is_host ? "YOU ARE THE HOST" : "YOU ARE THE GUEST");

    const int seats = s.max_slots > 4 ? 4 : s.max_slots;
    panel(g, 24.0f, 76.0f, 340.0f, 150.0f, "PILOTS");
    for (int i = 0; i < seats; ++i) {
        const float y = 90.0f + 32.0f * i;
        char buf[96];
        std::snprintf(buf, sizeof buf, "P%d", i + 1);
        text(g, 40.0f, y + 6.0f, 13.0f, kDim, buf);
        if (!s.occupied[i]) {
            text(g, 76.0f, y + 6.0f, 13.0f, kOff, "- OPEN -");
            continue;
        }
        const RecompLauncherCNetplayMember& mem = s.slots[i];
        if (mem.is_local)
            g.dl->AddRectFilledMultiColor(at(g, 25.0f, y), at(g, 363.0f, y + kRowH),
                                          IM_COL32(95, 224, 138, 40),
                                          IM_COL32(95, 224, 138, 0),
                                          IM_COL32(95, 224, 138, 0),
                                          IM_COL32(95, 224, 138, 40));
        std::string name = upper(mem.display_name);
        if (name.size() > 18) name = name.substr(0, 17) + ".";
        text(g, 76.0f, y + 6.0f, 13.0f, mem.is_local ? kBright : kText, name.c_str());
        if (mem.is_host) {
            lamp(g, 350.0f, y + 5.0f, true, "HOST");
        } else {
            const bool ready = mem.ready &&
                               !(mem.mod_readiness_valid && mem.mods_missing > 0);
            lamp(g, 350.0f, y + 5.0f, ready, ready ? "RDY" : "WAIT");
            if (mem.latency_ms >= 0 && !mem.is_local) {
                std::snprintf(buf, sizeof buf, "%d MS", mem.latency_ms);
                text_right(g, 302.0f, y + 8.0f, 10.0f, kDim, buf);
            }
        }
    }

    panel(g, 24.0f, 242.0f, 340.0f, 174.0f, "COMMAND");
    int clicked = -1;
    for (int i = 0; i < count; ++i) {
        bool hovered = false;
        const bool enabled = entries[i].id != LOBBY_START || can_start;
        if (row(g, 25.0f, 252.0f + kRowH * i, 338.0f, i == cur, enabled,
                entries[i].label, entries[i].toggle ? nullptr : ">",
                entries[i].toggle, entries[i].on, &hovered))
            clicked = i;
        /* A greyed START still takes the cursor, so its reason can be read. */
        if (hover_moved(hovered)) cur = i;
    }

    panel(g, 380.0f, 76.0f, 236.0f, 340.0f, "ROOM");
    {
        float y = 92.0f;
        char buf[128];
        text(g, 394.0f, y, 10.0f, kDim, "MATCH");
        text(g, 474.0f, y, 10.0f, kText, "LINK VERSUS");
        y += 17.0f;
        text(g, 394.0f, y, 10.0f, kDim, "PILOTS");
        std::snprintf(buf, sizeof buf, "%d OF %d", s.seated_players, s.max_slots);
        text(g, 474.0f, y, 10.0f, kText, buf);
        y += 17.0f;
        if (m->netplay_local_room && m->netplay_host_endpoint[0]) {
            text(g, 394.0f, y, 10.0f, kDim, "ADDRESS");
            text(g, 474.0f, y, 10.0f, kBright, m->netplay_host_endpoint);
            y += 17.0f;
        }
        if (card_known) {
            text(g, 394.0f, y, 10.0f, kDim, "P2 CARD");
            text(g, 474.0f, y, 10.0f, kText,
                 card_shared && card_allowed ? "GUEST'S OWN" : "HOST'S SLOT 2");
            y += 17.0f;
        }
        y += 12.0f;
        if (waiting) {
            text(g, 394.0f, y, 11.0f, kHi, "STANDING BY");
            text(g, 394.0f, y + 20.0f, 11.0f, kText, waiting, 208.0f);
        } else if (s.is_host) {
            text(g, 394.0f, y, 11.0f, kOn, "READY");
            text(g, 394.0f, y + 20.0f, 11.0f, kText,
                 "Both pilots are in. Start the match when you are ready.", 208.0f);
        } else {
            text(g, 394.0f, y, 11.0f, kOn, "READY");
            text(g, 394.0f, y + 20.0f, 11.0f, kText,
                 "Waiting for the host to start the match.", 208.0f);
        }
        if (s.is_host && s.seated_players < 2 && m->netplay_local_room)
            text(g, 394.0f, y + 64.0f, 10.0f, kDim,
                 "Pilots on this network see the room in their list. Anyone "
                 "else can join by the address above.", 208.0f);
        if (m->netplay_status[0])
            text(g, 394.0f, 330.0f, 10.0f, kWarn, m->netplay_status, 208.0f);
    }
    footer(g, s.is_host ? "A / ENTER  SELECT      START  START MATCH      B / ESC  LEAVE"
                        : "A / ENTER  SELECT      B / ESC  LEAVE");

    moa_menu_lobby_popups(m, th, np, s);

    int activate = -1;
    if (clicked >= 0) activate = cur = clicked;
    else if (in.confirm) activate = cur;
    else if (in.start && s.is_host) activate = 0;
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
        case LOBBY_START:
            if (can_start) np_lobby_start(m, np);
            else note("NOT READY TO START YET");
            break;
        case LOBBY_CARD:
            if (s.is_host) {
                if (np->guest_memcard_set)
                    (void)np->guest_memcard_set(np->ctx, card_allowed ? 0 : 1);
            } else {
                (void)np->memcard_offer_set(np->ctx, has_card > 0 ? 1 : 0,
                                            card_shared ? 0 : 1);
            }
            break;
        case LOBBY_SETTINGS:
            g_lobby_settings_synced = false;
            m->netplay_lobby_settings_open = true;
            break;
        case LOBBY_MODS:
            m->netplay_lobby_mods_open = true;
            break;
        case LOBBY_LEAVE:
            g_state.cursor[PAGE_LOBBY] = 0;
            np_lobby_leave(m, np);
            break;
    }
}
