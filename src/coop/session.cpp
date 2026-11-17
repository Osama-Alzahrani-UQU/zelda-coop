#include "session.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <random>
#include <thread>

#include "common.h"
#include "config.h"
#include "coop.h"
#include "menus.h"

namespace coop {

using net::Addr;
using net::Reader;
using net::Writer;

namespace {

enum Kind : uint8_t { kReliable = 0, kUnreliable = 1, kAckOnly = 2, kDiscovery = 3 };
enum Msg : uint8_t {
    M_HELLO = 1, M_WELCOME, M_REJECT, M_LOBBY, M_READY, M_START, M_STATE, M_PING, M_PONG, M_BYE, M_TOAST,
    M_GIFT_OFFER, M_GIFT_REPLY, M_GIFT_SEND, M_GIFT_DONE, M_KILL, M_KICK, M_SAVE_ALL, M_REVIVE, M_RESPAWN, M_POSE, M_STARTER, M_GEAR, M_ENEMY, M_EHIT, M_WFLAG,
    M_WORLD_ASK, M_WORLD_PIECE, M_ITEMS_KEEP, M_ITEMS_BACK, M_OBJHIT,
    M_BEACON = 100, M_PRESENCE, M_INVITE
};
constexpr int kHeader = 20;
constexpr char kModVersion[] = "1.1.0";
constexpr int64_t kTimeoutMs = 7000;

struct Pending {
    uint32_t seq;
    std::vector<uint8_t> msg;   // (type + payload)
    int64_t sent_ms;
    int sends;
};

struct Conn {
    bool used = false;
    Addr addr;
    int slot = -1;
    uint32_t out_seq = 0;
    std::deque<Pending> unacked;
    uint32_t in_next = 1;                               // the next reliable seq expected
    std::map<uint32_t, std::vector<uint8_t>> early;    // reliable messages ahead of it
    bool ack_due = false;
    int64_t last_recv_ms = 0, last_send_ms = 0;
    int rtt = -1;
    uint32_t state_seq_seen = 0;
};

uint32_t random_u32() {
    static std::mt19937 rng((uint32_t)GetTickCount64() ^ (uint32_t)GetCurrentProcessId() * 2654435761u);
    return rng();
}

}  // namespace

struct Session::Impl {
    std::mutex m;
    std::thread th;
    std::atomic<bool> running{true};
    net::Socket play, find;
    Role role = Role::None;
    Phase phase = Phase::Idle;
    uint32_t session = 0;
    int me = -1;
    int host_file = -1;
    bool server_running = false;
    bool searching = false;
    std::string name = "Link", status, host_ip;
    HostSettings hs;
    GuestSettings gs;
    std::string server_name;   // (as a guest: the host's)
    Player players[kMaxPlayers];
    Conn conns[kMaxPlayers];   // host: one per guest slot; guest: conns[0] is the host
    std::string join_password;
    int64_t join_started_ms = 0;
    std::vector<ServerEntry> servers;
    std::vector<FriendEntry> friends;
    std::deque<Toast> toasts;
    std::deque<GameAction> actions;
    std::atomic<int> n_actions{0};   // (actions' size, read by the game's threads without the lock)
    std::map<uint32_t, Gift> gifts;
    std::vector<KillEvent> kills;       // (the host: every kill of the session, for players who join later)
    std::vector<WorldFlag> world_in;    // (placed objects' state changed in the others' games, not taken yet)
    // (the host's save for a guest: the guests that asked; the saves on their way, a piece at a time as the connection
    // takes them; a guest: the save as it comes)
    std::vector<int> world_asks;
    struct WorldOut {
        int to = -1;
        uint32_t xfer = 0;
        std::vector<uint8_t> file[2];
        int f = 0;
        uint32_t at = 0;
    };
    std::vector<WorldOut> world_out;
    struct WorldIn {
        uint32_t xfer = 0;
        bool none = false;
        std::vector<uint8_t> file[2];
        std::vector<uint8_t> got[2];
        uint32_t total[2] = {0, 0}, have[2] = {0, 0};
        bool sized[2] = {false, false};
    } world;
    std::vector<std::pair<std::string, std::string>> items_kept;   // (the host: guests' items to keep, by name)
    std::string items_back;                                        // (a guest: its own items, from the host)
    bool items_back_new = false;
    std::vector<KillEvent> new_kills;   // (the others' kills, for this game to carry out)
    std::atomic<int> n_new_kills{0};
    std::atomic<bool> respawn_asked{false};   // (for the game: forget the kills not done yet)
    int64_t save_seen = 0, next_save_check = 0, last_save_all = 0;   // (the host's saves, shared)
    PlayerState local;
    bool local_new = false;
    uint32_t local_seq = 0;
    // (Link's pose: this game's newest, sent unreliable; the others' newest, for the game's frame)
    struct PoseIn {
        std::vector<uint8_t> blob;
        uint32_t seq = 0;
        int64_t got_ms = 0;
    };
    std::vector<uint8_t> local_pose;
    bool pose_new = false;
    uint32_t pose_seq = 0;
    int64_t last_pose_send = 0;
    PoseIn poses[kMaxPlayers];
    std::vector<std::pair<std::string, int>> starter;   // (the host's starting gear)
    std::vector<EnemySnap> enemy_in;   // (the others' shared enemies, newest of each)
    std::string local_gear;
    bool gear_new = false;
    int64_t last_gear_send = 0;
    int64_t next_beacon = 0, next_ping = 0, next_lobby = 0, last_state_send = 0;

    void act(GameAction a) {   // (something for the game's thread: under the lock)
        actions.push_back(std::move(a));
        n_actions.fetch_add(1, std::memory_order_release);
    }
    void add_toast(const std::string& t, int kind = 0) {
        toasts.push_back({t, kind, net::now_ms()});
        while (toasts.size() > 6) toasts.pop_front();
        log("toast: %s", t.c_str());
    }

    // ---- packets
    std::vector<uint8_t> packet(Kind kind, uint32_t seq, uint32_t ack, const std::vector<uint8_t>& msg) {
        Writer w;
        w.u32(net::kMagic);
        w.u16(net::kProtocol);
        w.u8(kind);
        w.u8((uint8_t)(me < 0 ? 255 : me));
        w.u32(session);
        w.u32(seq);
        w.u32(ack);
        w.raw(msg.data(), msg.size());
        return w.buf;
    }
    void send_raw(Conn& c, Kind kind, uint32_t seq, const std::vector<uint8_t>& msg) {
        auto p = packet(kind, seq, c.in_next - 1, msg);
        play.send(c.addr, p.data(), p.size());
        c.last_send_ms = net::now_ms();
        c.ack_due = false;
    }
    void send_reliable(Conn& c, const std::vector<uint8_t>& msg) {
        if (!c.used) return;
        Pending p{++c.out_seq, msg, net::now_ms(), 1};
        send_raw(c, kReliable, p.seq, msg);
        c.unacked.push_back(std::move(p));
    }
    void send_unreliable(Conn& c, const std::vector<uint8_t>& msg) {
        if (c.used) send_raw(c, kUnreliable, 0, msg);
    }
    void to_all(const std::vector<uint8_t>& msg, bool reliable, int except = -1) {
        for (auto& c : conns)
            if (c.used && c.slot != except) reliable ? send_reliable(c, msg) : send_unreliable(c, msg);
    }
    Conn* conn_of_slot(int slot) {
        for (auto& c : conns)
            if (c.used && c.slot == slot) return &c;
        return nullptr;
    }
    Conn* conn_of_addr(const Addr& a) {
        for (auto& c : conns)
            if (c.used && c.addr == a) return &c;
        return nullptr;
    }

    // ---- messages
    std::vector<uint8_t> msg_lobby() {
        Writer w;
        w.u8(M_LOBBY);
        w.u8((uint8_t)phase);
        w.i32(host_file);
        w.str(hs.server);
        w.u8(hs.share_kills), w.u8(hs.gifts), w.u8(hs.same_age), w.u8(hs.join_in_progress);
        w.u8(hs.revive), w.u8(hs.respawn_for_new), w.u8(hs.starter);
        for (auto& p : players) {
            w.u8(p.used);
            w.str(p.name);
            w.u8(p.host), w.u8(p.ready);
            w.i32(p.ping);
        }
        return w.buf;
    }
    std::vector<uint8_t> msg_state(int slot, const PlayerState& s) {
        Writer w;
        w.u8(M_STATE);
        w.u8((uint8_t)slot);
        w.u32(s.seq);
        for (float v : s.pos) w.f32(v);
        w.f32(s.yaw), w.f32(s.speed);
        w.u16((uint16_t)s.life), w.u16((uint16_t)s.life_max);
        w.u8(s.child), w.u8(s.flags);
        w.str(s.map);
        w.f32(s.cam_yaw);
        return w.buf;
    }
    static std::vector<uint8_t> msg_kill(const KillEvent& k) {
        Writer w;
        w.u8(M_KILL);
        w.u32(k.id);
        w.str(k.actor);
        for (float v : k.home) w.f32(v);
        w.str(k.map);
        if (k.has_spawn)
            for (float v : k.spawn) w.f32(v);
        return w.buf;
    }
    void on_respawn(const std::string& who) {
        new_kills.clear();
        n_new_kills = 0;
        respawn_asked = true;
        add_toast(who + " joined: the enemies come back for everyone", 0);
    }
    void kill_in(const KillEvent& k) {   // (a kill for this game; once each)
        for (const auto& o : new_kills)
            if (o.id == k.id) return;
        new_kills.push_back(k);
        if (new_kills.size() > 2048) new_kills.erase(new_kills.begin(), new_kills.begin() + 512);
        n_new_kills.store((int)new_kills.size(), std::memory_order_release);
    }
    void kill_log(const KillEvent& k) {   // (the host's record of the session's kills)
        if (role != Role::Host) return;
        for (const auto& o : kills)
            if (o.id == k.id) return;
        kills.push_back(k);
        if (kills.size() > 4096) kills.erase(kills.begin(), kills.begin() + 1024);
    }
    static std::vector<uint8_t> msg_toast(const std::string& t, int kind) {
        Writer w;
        w.u8(M_TOAST);
        w.str(t);
        w.u8((uint8_t)kind);
        return w.buf;
    }
    std::vector<uint8_t> msg_gift(Msg type, const Gift& g) {
        Writer w;
        w.u8(type);
        w.u32(g.id);
        w.u8((uint8_t)g.from), w.u8((uint8_t)g.to);
        w.str(g.item), w.str(g.label);
        w.i32(g.have), w.i32(g.amount);
        w.u8((uint8_t)g.state);
        return w.buf;
    }
    static bool read_gift(Reader& r, Gift& g) {
        g.id = r.u32();
        g.from = r.u8(), g.to = r.u8();
        g.item = r.str(), g.label = r.str();
        g.have = r.i32(), g.amount = r.i32();
        g.state = r.u8();
        return r.ok();
    }

    // ---- lifecycle
    void reset_players() {
        for (auto& p : players) p = Player{};
        for (auto& c : conns) c = Conn{};
        for (auto& q : poses) q = PoseIn{};
    }
    void open_play(uint16_t port) {
        if (!play.open(port, false, false)) {
            // (another copy of the game on this PC: any port; guests find it by its beacon)
            play.open(0, false, false);
        }
    }

    bool host_start(const HostSettings& s, int file) {
        if (!net::startup()) return false;
        hs = s;
        host_file = file;
        role = Role::Host;
        phase = Phase::Lobby;
        session = random_u32() | 1;
        reset_players();
        me = 0;
        players[0].used = true, players[0].me = true, players[0].host = true, players[0].ready = true;
        players[0].name = name;
        open_play(hs.port);
        server_running = play.is_open();
        const uint32_t ip = net::preferred_ip();
        host_ip = Addr{ip, play.port()}.str();
        status = server_running ? "running" : "could not open the network port";
        log("session: hosting \"%s\" on %s (file %d)", hs.server.c_str(), host_ip.c_str(), file);
        add_toast("Server started: " + host_ip, 1);
        next_beacon = 0;
        return server_running;
    }
    void host_stop() {
        if (role != Role::Host) return;
        if (phase == Phase::Playing || phase == Phase::Starting) game_save_now("the server closing");
        Writer w;
        w.u8(M_KICK);
        w.str("The host stopped the server");
        to_all(w.buf, true);
        flush_some();
        for (int i = 1; i < kMaxPlayers; i++) {
            if (players[i].used) add_toast(players[i].name + " left", 2);
            players[i] = Player{};
            conns[i] = Conn{};
        }
        play.close();
        server_running = false;
        status = "stopped";
        add_toast("Server stopped", 2);
    }
    bool host_resume() {
        if (role != Role::Host || server_running) return server_running;
        open_play(hs.port);
        server_running = play.is_open();
        host_ip = Addr{net::preferred_ip(), play.port()}.str();
        status = server_running ? "running" : "could not open the network port";
        if (server_running) add_toast("Server started: " + host_ip, 1);
        return server_running;
    }
    bool join(const Addr& a, const std::string& pw) {
        if (!net::startup()) return false;
        reset_players();
        role = Role::Guest;
        phase = Phase::Connecting;
        session = 0;
        me = -1;
        join_password = pw;
        open_play(0);
        conns[0].used = true, conns[0].addr = a, conns[0].slot = 0, conns[0].last_recv_ms = net::now_ms();
        join_started_ms = net::now_ms();
        status = "connecting";
        host_ip = a.str();
        Writer w;
        w.u8(M_HELLO);
        w.str(name), w.str(pw), w.str(kModVersion);
        send_reliable(conns[0], w.buf);
        log("session: joining %s", a.str().c_str());
        return true;
    }
    void leave() {
        if (role == Role::None) return;
        if (phase == Phase::Playing || phase == Phase::Starting) game_save_now(role == Role::Host ? "the server closing" : "leaving the session");
        Writer w;
        w.u8(M_BYE);
        to_all(w.buf, true);
        flush_some();
        play.close();
        role = Role::None;
        phase = Phase::Idle;
        server_running = false;
        me = -1;
        reset_players();
        gifts.clear();
        kills.clear();
        new_kills.clear();
        world_asks.clear();
        world_out.clear();
        world = WorldIn{};
        items_kept.clear();
        items_back_new = false;
        n_new_kills = 0;
        save_seen = 0;
        status.clear();
        log("session: left");
    }
    void flush_some() {
        // (the last reliable messages pushed out a few times before the socket closes)
        for (int k = 0; k < 3; k++)
            for (auto& c : conns)
                for (auto& p : c.unacked) send_raw(c, kReliable, p.seq, p.msg);
    }

    // ---- receiving
    void on_packet(const Addr& from, const uint8_t* data, int n) {
        if (n < kHeader) return;
        Reader r(data, (size_t)n);
        if (r.u32() != net::kMagic || r.u16() != net::kProtocol) return;
        const Kind kind = (Kind)r.u8();
        const int from_slot = r.u8();
        const uint32_t sess = r.u32(), seq = r.u32(), ack = r.u32();
        (void)from_slot;
        Conn* c = conn_of_addr(from);
        if (role == Role::Host && !c) {
            // (a new guest: only its HELLO opens a connection)
            if (kind != kReliable || r.left() < 1 || data[kHeader] != M_HELLO || seq != 1) return;
            c = accept_guest(from);
            if (!c) return;
        }
        if (!c) return;
        if (role == Role::Guest && session && sess != session && sess != 0) return;
        c->last_recv_ms = net::now_ms();
        while (!c->unacked.empty() && (int32_t)(c->unacked.front().seq - ack) <= 0) {
            if (c->unacked.front().sends == 1) {
                const int sample = (int)(net::now_ms() - c->unacked.front().sent_ms);
                c->rtt = c->rtt < 0 ? sample : (c->rtt * 7 + sample) / 8;
            }
            c->unacked.pop_front();
        }
        const uint8_t* msg = data + kHeader;
        const size_t len = (size_t)n - kHeader;
        if (kind == kUnreliable) {
            on_message(*c, msg, len);
        } else if (kind == kReliable) {
            c->ack_due = true;
            if (seq == c->in_next) {
                c->in_next++;
                on_message(*c, msg, len);
                for (auto it = c->early.find(c->in_next); it != c->early.end(); it = c->early.find(c->in_next)) {
                    auto m2 = std::move(it->second);
                    c->early.erase(it);
                    c->in_next++;
                    if (!conn_alive(c)) return;
                    on_message(*c, m2.data(), m2.size());
                }
            } else if ((int32_t)(seq - c->in_next) > 0 && c->early.size() < 512) {
                c->early.emplace(seq, std::vector<uint8_t>(msg, msg + len));
            }
        }
    }
    bool conn_alive(Conn* c) { return c->used; }

    Conn* accept_guest(const Addr& from) {
        if (!server_running) return nullptr;
        int slot = -1;
        int count = 0;
        for (int i = 0; i < kMaxPlayers; i++) count += players[i].used;
        for (int i = 1; i < kMaxPlayers; i++)
            if (!players[i].used && !conns[i].used) { slot = i; break; }
        if (slot < 0 || count >= hs.max_players || (phase == Phase::Playing && !hs.join_in_progress)) {
            // (refused at once, without a connection: a reliable reply would wait for an ack it never gets)
            Writer w;
            w.u8(M_REJECT);
            w.str(slot < 0 || count >= hs.max_players ? "The server is full" : "The game has already started");
            Conn tmp;
            tmp.used = true, tmp.addr = from;
            send_raw(tmp, kReliable, 1, w.buf);
            return nullptr;
        }
        Conn& c = conns[slot];
        c = Conn{};
        c.used = true, c.addr = from, c.slot = slot, c.last_recv_ms = net::now_ms();
        return &c;
    }

    void drop(Conn& c, const std::string& why) {
        const int slot = c.slot;
        if (role == Role::Host) {
            if (slot > 0 && players[slot].used) add_toast(players[slot].name + " left" + (why.empty() ? "" : " (" + why + ")"), 2);
            if (slot > 0) players[slot] = Player{}, poses[slot] = PoseIn{};
            c = Conn{};
            for (auto& [id, g] : gifts)
                if ((g.from == slot || g.to == slot) && g.state < kGiftDone) g.state = kGiftFailed;
            next_lobby = 0;
        } else {
            add_toast(why.empty() ? "Disconnected from the host" : why, 2);
            leave();
        }
    }

    void on_message(Conn& c, const uint8_t* msg, size_t len) {
        if (!len) return;
        Reader r(msg + 1, len - 1);
        switch (msg[0]) {
        case M_HELLO: {
            if (role != Role::Host) return;
            const std::string nm = r.str(), pw = r.str(), ver = r.str();
            const int slot = c.slot;
            if (ver != kModVersion) {   // (another version of the mod: its messages are not the same)
                Writer w;
                w.u8(M_REJECT);
                w.str("Different mod versions (host " + std::string(kModVersion) + ", you " + (ver.empty() ? std::string("?") : ver) +
                      "): install the same Longshot Co-op on both PCs");
                send_reliable(c, w.buf);
                flush_some();
                c = Conn{};
                return;
            }
            if (!hs.password.empty() && pw != hs.password) {
                Writer w;
                w.u8(M_REJECT);
                w.str("Wrong password");
                send_reliable(c, w.buf);
                flush_some();
                c = Conn{};
                return;
            }
            players[slot] = Player{};
            players[slot].used = true;
            // (each player's own name; one already taken gets the slot's number: two "Link"s are told apart)
            std::string name = nm.empty() ? "Player " + std::to_string(slot + 1) : nm.substr(0, 24);
            for (int i = 0; i < kMaxPlayers; i++)
                if (i != slot && players[i].used && players[i].name == name) name += " " + std::to_string(slot + 1);
            players[slot].name = name;
            Writer w;
            w.u8(M_WELCOME);
            w.u8((uint8_t)slot);
            w.u32(session);
            w.str(hs.server);
            send_reliable(c, w.buf);
            add_toast(players[slot].name + " joined", 1);
            log("session: %s joined in slot %d from %s (mod %s)", players[slot].name.c_str(), slot, c.addr.str().c_str(), ver.c_str());
            next_lobby = 0;
            if (phase == Phase::Playing || phase == Phase::Starting) {
                Writer s;
                s.u8(M_START);
                s.i32(host_file == -2 && phase == Phase::Starting ? -2 : 0);   // (-2: a new game, not begun yet: one too)
                send_reliable(c, s.buf);
                // (joining a game in progress: the enemies already defeated stay defeated for them; or, with the host's
                // option, they come back for everyone: the record forgotten, every game told)
                if (hs.share_kills && !hs.respawn_for_new) {
                    for (const auto& k : kills) send_reliable(c, msg_kill(k));
                } else if (hs.share_kills) {
                    kills.clear();
                    Writer f;
                    f.u8(M_RESPAWN);
                    f.str(players[slot].name);
                    to_all(f.buf, true, slot);
                    on_respawn(players[slot].name);
                }
            }
            break;
        }
        case M_WELCOME: {
            if (role != Role::Guest) return;
            me = r.u8();
            session = r.u32();
            server_name = r.str();
            phase = Phase::Lobby;
            status = "connected";
            players[me].me = true;
            add_toast("Joined " + server_name, 1);
            log("session: joined \"%s\" as slot %d", server_name.c_str(), me);
            break;
        }
        case M_REJECT: {
            const std::string why = r.str();
            add_toast("Could not join: " + why, 2);
            leave();
            break;
        }
        case M_KICK: {
            const std::string why = r.str();
            add_toast(why.empty() ? "Removed from the session" : why, 2);
            leave();
            break;
        }
        case M_LOBBY: {
            if (role != Role::Guest) return;
            const Phase ph = (Phase)r.u8();
            host_file = r.i32();
            hs.server = r.str();
            hs.share_kills = r.u8(), hs.gifts = r.u8(), hs.same_age = r.u8(), hs.join_in_progress = r.u8();
            hs.revive = r.u8(), hs.respawn_for_new = r.u8(), hs.starter = r.u8();
            for (int i = 0; i < kMaxPlayers; i++) {
                Player& p = players[i];
                const bool used = r.u8();
                std::string nm = r.str();
                const bool host = r.u8(), ready = r.u8();
                const int ping = r.i32();
                if (!r.ok()) return;
                if (used && !p.used) p = Player{};
                p.used = used, p.name = nm, p.host = host, p.ping = ping, p.me = i == me;
                if (i != me) p.ready = ready;
            }
            if (ph == Phase::Lobby && phase != Phase::Connecting) phase = Phase::Lobby;
            break;
        }
        case M_READY: {
            if (role != Role::Host) return;
            players[c.slot].ready = r.u8();
            next_lobby = 0;
            break;
        }
        case M_START: {
            if (role != Role::Guest) return;
            phase = Phase::Starting;
            GameAction a{GameAction::StartGame};
            a.n = r.i32();   // (-2: a new game, as the host's; else the host's save, asked for)
            if (!r.ok()) a.n = 0;
            act(a);
            add_toast("The host started the game", 1);
            break;
        }
        case M_STATE: {
            const int slot = r.u8();
            if (slot >= kMaxPlayers) return;
            PlayerState s;
            s.seq = r.u32();
            for (float& v : s.pos) v = r.f32();
            s.yaw = r.f32(), s.speed = r.f32();
            s.life = (int16_t)r.u16(), s.life_max = (int16_t)r.u16();
            s.child = r.u8(), s.flags = r.u8();
            const std::string mp = r.str();
            s.cam_yaw = r.f32();
            if (!r.ok()) return;
            snprintf(s.map, sizeof s.map, "%s", mp.c_str());
            s.got_ms = net::now_ms();
            if (role == Role::Host && slot != c.slot) return;   // (a guest speaks for itself only)
            Player& p = players[slot];
            if (!p.used || (int32_t)(s.seq - p.st.seq) <= 0) return;
            p.prev = p.st;
            p.st = s;
            if (role == Role::Host) to_all(std::vector<uint8_t>(msg, msg + len), false, slot);
            // (a player in the game for the first time: the host's starting gear, for what they do not have yet)
            // (and the host's bags (sticks, nuts, seeds, bombs, arrows, the wallet) whenever they change: without its bag a
            // game holds none of that kind, a gift and a pick-up refused)
            if (role == Role::Host && (s.flags & kInPlay) && !starter.empty()) {
                std::vector<std::pair<std::string, int>> gear, bagv, out;
                uint32_t bags = 2166136261u;
                for (const auto& it : starter) {
                    const bool bag = it.first.find("Bag_") != std::string::npos || it.first.find("Quiver_") != std::string::npos ||
                                     it.first.find("Wallet_") != std::string::npos;
                    (bag ? bagv : gear).push_back(it);
                    if (bag)
                        for (char ch : it.first) bags = (bags ^ (uint8_t)ch) * 16777619u;
                }
                if (!p.starter_sent) out = gear;   // (the gear once, as the player first plays)
                if (bags != p.bags_sent) out.insert(out.end(), bagv.begin(), bagv.end());   // (the bags as they grow)
                if (!out.empty()) {
                    Writer w;
                    w.u8(M_STARTER);
                    w.u8((uint8_t)out.size());
                    for (const auto& [item, n] : out) w.str(item), w.u16((uint16_t)std::clamp(n, 1, 999));
                    send_reliable(c, w.buf);
                    log("session: the starting gear and bags (%zu kinds) sent to %s", out.size(), p.name.c_str());
                }
                p.starter_sent = true;
                p.bags_sent = bags;
            }
            break;
        }
        case M_ENEMY: {   // (a shared enemy as the game running it sees it; relayed by the host)
            EnemySnap e;
            e.owner = r.u8();
            e.map = r.str();
            e.name = r.str();
            e.id = r.u32();
            e.lead = r.u8() != 0;
            for (float& v : e.home) v = r.f32();
            for (float& v : e.pos) v = r.f32();
            for (float& v : e.rot) v = r.f32();
            const uint16_t n = r.u16();
            if (!r.ok() || e.owner >= kMaxPlayers || n > r.left() || n > 1800) return;
            if (role == Role::Host && e.owner != c.slot) return;
            e.pose.resize(n);
            r.raw(e.pose.data(), n);
            e.got_ms = net::now_ms();
            bool put = false;
            for (auto& o : enemy_in) {
                if (o.owner == e.owner && o.id == e.id && o.map == e.map) {
                    o = e;
                    put = true;
                    break;
                }
            }
            if (!put) {
                enemy_in.erase(std::remove_if(enemy_in.begin(), enemy_in.end(),
                                              [&](const EnemySnap& o) { return e.got_ms - o.got_ms > 3000; }),
                               enemy_in.end());
                if (enemy_in.size() < 64) enemy_in.push_back(e);
            }
            if (role == Role::Host) to_all(std::vector<uint8_t>(msg, msg + len), false, e.owner);
            break;
        }
        case M_WFLAG: {   // (placed objects' state changed in another game; relayed by the host)
            const int from = r.u8();
            const std::string mp = r.str();
            const uint16_t n = r.u16();
            if (!r.ok() || from >= kMaxPlayers || n > 512 || r.left() < (size_t)n * 12) return;
            if (role == Role::Host && from != c.slot) return;
            for (int i = 0; i < n; i++) {
                WorldFlag f;
                f.key = r.u64();
                f.value = r.u32();
                if (from != me && world_in.size() < 8192) world_in.push_back(f);
            }
            if (role == Role::Host) to_all(std::vector<uint8_t>(msg, msg + len), true, from);
            break;
        }
        case M_WORLD_ASK: {   // (a guest wants the host's save)
            if (role != Role::Host || c.slot <= 0 || !players[c.slot].used) return;
            if (std::find(world_asks.begin(), world_asks.end(), c.slot) == world_asks.end()) world_asks.push_back(c.slot);
            world_out.erase(std::remove_if(world_out.begin(), world_out.end(), [&](const WorldOut& o) { return o.to == c.slot; }),
                            world_out.end());
            log("session: %s asked for the host's world", players[c.slot].name.c_str());
            break;
        }
        case M_WORLD_PIECE: {   // (a piece of the host's save: file 0 the caption, 1 the progress, 2 none)
            if (role != Role::Guest) return;
            const uint32_t xfer = r.u32();
            const uint8_t f = r.u8();
            const uint32_t total = r.u32(), at = r.u32();
            const uint16_t n = r.u16();
            if (!r.ok() || f > 2) return;
            if (xfer != world.xfer) world = WorldIn{}, world.xfer = xfer;
            if (f == 2) {
                world.none = true;
                break;
            }
            if (total > (8u << 20) || n > r.left() || (uint64_t)at + n > total || (at % 1024) != 0) return;
            if (!world.sized[f] || world.total[f] != total) {
                world.sized[f] = true;
                world.total[f] = total, world.have[f] = 0;
                world.file[f].assign(total, 0);
                world.got[f].assign((total + 1023) / 1024, 0);
            }
            const uint32_t piece = at / 1024;
            if (piece < world.got[f].size() && !world.got[f][piece]) {
                r.raw(world.file[f].data() + at, n);
                world.got[f][piece] = 1;
                world.have[f] += n;
            }
            break;
        }
        case M_ITEMS_KEEP: {   // (a guest's items, for the host to keep under its name)
            if (role != Role::Host || c.slot <= 0 || !players[c.slot].used) return;
            const std::string list = r.str();
            if (!r.ok() || list.size() > 8000) return;
            if (items_kept.size() < 32) items_kept.push_back({players[c.slot].name, list});
            break;
        }
        case M_ITEMS_BACK: {   // (this guest's own items, kept by the host)
            if (role != Role::Guest) return;
            const std::string list = r.str();
            if (!r.ok()) return;
            items_back = list, items_back_new = true;
            log("session: the host gave back this player's items (%zu bytes)", list.size());
            break;
        }
        case M_OBJHIT: {   // (a blow on a placed object in another game: the same object here)
            const int from = r.u8();
            GameAction a{GameAction::ObjectHit};
            const std::string mp = r.str(), nm = r.str();
            a.key = r.u64();
            a.n = r.u16();
            if (!r.ok() || from >= kMaxPlayers || !a.key || a.n <= 0) return;
            if (role == Role::Host && from != c.slot) return;
            a.s = mp + '\n' + nm;
            if (from != me) act(a);
            if (role == Role::Host) to_all(std::vector<uint8_t>(msg, msg + len), true, from);
            break;
        }
        case M_EHIT: {   // (a hit on a shared enemy in another game: the same life taken here)
            const int from = r.u8();
            GameAction a{GameAction::EnemyHit};
            const std::string mp = r.str(), nm = r.str();
            for (float& v : a.pos) v = r.f32();
            a.n = r.u16();
            const int owner = r.u8();
            const uint32_t id = r.u32();
            if (!r.ok() || from >= kMaxPlayers) return;
            a.key = ((uint64_t)(owner < kMaxPlayers ? owner : from) << 32) | id;
            if (role == Role::Host && from != c.slot) return;
            a.s = mp + '\n' + nm;
            if (from != me) act(a);
            if (role == Role::Host) to_all(std::vector<uint8_t>(msg, msg + len), true, from);
            break;
        }
        case M_GEAR: {   // (what a player's Link wears; relayed by the host)
            const int slot = r.u8();
            const std::string g = r.str();
            if (slot >= kMaxPlayers || !r.ok() || g.size() > 400) return;
            if (role == Role::Host && slot != c.slot) return;
            if (!players[slot].used) return;
            players[slot].gear = g;
            if (role == Role::Host) to_all(std::vector<uint8_t>(msg, msg + len), false, slot);
            break;
        }
        case M_STARTER: {   // (the host's starting gear: this game takes what it does not have yet)
            if (role != Role::Guest) return;
            const int n = r.u8();
            for (int i = 0; i < n; i++) {
                GameAction a{GameAction::Starter};
                a.s = r.str();
                a.n = r.u16();
                if (!r.ok()) break;
                if (!a.s.empty() && a.n > 0) act(a);
            }
            break;
        }
        case M_POSE: {   // (a player's Link's pose: newest wins, relayed by the host)
            const int slot = r.u8();
            const uint32_t seq = r.u32();
            const uint16_t n = r.u16();
            if (slot >= kMaxPlayers || !r.ok() || n > r.left() || n > 1800) return;
            if (role == Role::Host && slot != c.slot) return;
            if (!players[slot].used) return;
            PoseIn& in = poses[slot];
            if (in.seq && (int32_t)(seq - in.seq) <= 0) return;
            in.blob.resize(n);
            r.raw(in.blob.data(), n);
            in.seq = seq;
            in.got_ms = net::now_ms();
            if (role == Role::Host) to_all(std::vector<uint8_t>(msg, msg + len), false, slot);
            break;
        }
        case M_PING: {
            Writer w;
            w.u8(M_PONG);
            w.u32(r.u32());
            send_unreliable(c, w.buf);
            break;
        }
        case M_PONG: {
            const uint32_t t = r.u32();
            const int rtt = (int)((uint32_t)net::now_ms() - t);
            c.rtt = c.rtt < 0 ? rtt : (c.rtt * 3 + rtt) / 4;
            if (role == Role::Host) players[c.slot].ping = c.rtt;
            else if (me >= 0) players[me].ping = c.rtt;
            break;
        }
        case M_BYE:
            drop(c, "");
            break;
        case M_TOAST: {
            const std::string t = r.str();
            const int kind = r.u8();
            add_toast(t, kind);
            if (role == Role::Host) to_all(std::vector<uint8_t>(msg, msg + len), true, c.slot);
            break;
        }
        case M_GIFT_OFFER:
        case M_GIFT_REPLY:
        case M_GIFT_SEND:
        case M_GIFT_DONE: {
            Gift g;
            if (!read_gift(r, g)) return;
            if (role == Role::Host && g.to != 0 && g.from != 0) {   // (between two guests: passed on)
                const int other = c.slot == g.from ? g.to : g.from;   // (to the one of the two that did not send it)
                if (Conn* o = conn_of_slot(other)) send_reliable(*o, std::vector<uint8_t>(msg, msg + len));
            }
            on_gift((Msg)msg[0], g);
            break;
        }
        case M_KILL: {
            KillEvent k;
            k.id = r.u32();
            k.actor = r.str();
            for (float& v : k.home) v = r.f32();
            k.map = r.str();
            if (r.ok() && r.left() >= 12) {
                for (float& v : k.spawn) v = r.f32();
                k.has_spawn = r.ok();
            }
            if (!r.ok() || !k.id) return;
            log("session: a kill from slot %d: %s in %s", c.slot, k.actor.c_str(), k.map.c_str());
            if (role == Role::Host) {
                if (!hs.share_kills) return;
                to_all(std::vector<uint8_t>(msg, msg + len), true, c.slot);
                kill_log(k);
            }
            if (hs.share_kills) kill_in(k);
            break;
        }
        case M_RESPAWN: {   // (a player joined: the defeated enemies come back for everyone)
            if (role != Role::Guest) return;
            const std::string who = r.str();
            on_respawn(who);
            break;
        }
        case M_SAVE_ALL: {   // (the host's game saved: this one saves too)
            if (role != Role::Guest) return;
            act({GameAction::SaveNow});
            break;
        }
        case M_REVIVE: {   // (a player revived another: the host passes it on)
            const int target = r.u8(), from = r.u8();
            if (!r.ok() || target >= kMaxPlayers || from >= kMaxPlayers) return;
            if (role == Role::Host && target != me) {
                if (Conn* o = conn_of_slot(target)) send_reliable(*o, std::vector<uint8_t>(msg, msg + len));
                return;
            }
            if (target == me) {
                act({GameAction::Revive, players[from].name, from});
                add_toast(players[from].name + " revived you", 1);
            }
            break;
        }
        default:
            break;
        }
    }

    void on_gift(Msg type, const Gift& in) {
        Gift& g = gifts[in.id];
        const bool known = g.id != 0;
        if (!known) g = in;
        switch (type) {
        case M_GIFT_OFFER:
            if (in.to != me) return;
            g = in;
            g.state = kGiftOffered;
            g.at_ms = net::now_ms();
            if (!gs.gift_prompt) gift_reply(g.id, true);
            break;
        case M_GIFT_REPLY:
            if (in.from != me || !known) return;
            g.state = in.state;
            g.at_ms = net::now_ms();
            add_toast(players[g.to].name + (g.state == kGiftAccepted ? " accepted your gift" : " declined your gift"),
                      g.state == kGiftAccepted ? 1 : 2);
            break;
        case M_GIFT_SEND:
            if (in.to != me) return;
            g.amount = in.amount, g.item = in.item, g.label = in.label, g.state = kGiftSent;
            g.at_ms = net::now_ms();
            act({GameAction::AddItem, g.item, g.amount, g.id});
            break;
        case M_GIFT_DONE: {
            if (in.to == me && in.from != me) {   // (the giver called it off: its offer goes)
                if (known && g.state != kGiftDone) g.state = kGiftFailed, g.at_ms = net::now_ms();
                return;
            }
            if (in.from != me || !known) return;
            // (what the other game could not hold comes back into this one)
            const int sent = g.state == kGiftSent ? g.amount : 0;
            const int got = in.state == kGiftDone ? std::clamp(in.amount, 0, sent) : 0;
            g.state = in.state;
            g.at_ms = net::now_ms();
            const std::string who = in.to >= 0 && in.to < kMaxPlayers ? players[in.to].name : "The player";
            if (got > 0) add_toast(who + " received " + std::to_string(got) + "x " + g.label, 1);
            if (sent - got > 0) {
                act({GameAction::AddItem, g.item, sent - got, 0});
                add_toast(got > 0 ? std::to_string(sent - got) + " " + g.label + " came back: " + who + " had no room"
                                  : who + " cannot carry " + g.label + " yet: they came back to you",
                          2);
            } else if (got == 0) {
                add_toast("The gift could not be given", 2);
            }
            break;
        }
        default:
            break;
        }
    }
    Conn* route_to(int slot) {
        // (host: the slot's own connection; guest: everything goes to the host)
        if (role == Role::Guest) return conns[0].used ? &conns[0] : nullptr;
        return conn_of_slot(slot);
    }
    void gift_reply(uint32_t id, bool accept) {
        auto it = gifts.find(id);
        if (it == gifts.end()) return;
        Gift& g = it->second;
        g.state = accept ? kGiftAccepted : kGiftDeclined;
        if (Conn* c = route_to(g.from)) send_reliable(*c, msg_gift(M_GIFT_REPLY, g));
    }

    // ---- the thread's periodic work
    void tick(int64_t now) {
        // the host's game saved (its own save, or one asked): the guests' games save too
        if (role == Role::Host && phase == Phase::Playing && now >= next_save_check) {
            next_save_check = now + 2000;
            const int64_t t = newest_save_time();
            if (save_seen && t > save_seen && now - last_save_all > 20000) {
                Writer w;
                w.u8(M_SAVE_ALL);
                to_all(w.buf, true);
                last_save_all = now;
                log("session: the host's game saved: the guests' games save too");
            }
            if (t) save_seen = std::max(save_seen, t);
        }
        // discovery: hosts' beacons, searching guests' presence
        if (now >= next_beacon) {
            next_beacon = now + 1000;
            Writer w;
            bool send = false;
            if (role == Role::Host && server_running) {
                int count = 0;
                for (auto& p : players) count += p.used;
                w.u8(M_BEACON);
                w.str(hs.server), w.str(name);
                w.u8((uint8_t)count), w.u8((uint8_t)hs.max_players);
                w.u8(phase == Phase::Playing || phase == Phase::Starting);
                w.u8(!hs.password.empty());
                w.u16(play.port());
                w.u32(session);
                send = true;
            } else if (searching || role == Role::None) {
                w.u8(M_PRESENCE);
                w.str(name);
                w.u8(role != Role::None);
                send = searching;
            }
            if (send) {
                auto p = packet(kDiscovery, 0, 0, w.buf);
                for (const auto& f : net::interfaces()) find.send(Addr{f.bcast, net::kPortFind}, p.data(), p.size());
                find.send(Addr{0xFFFFFFFFu, net::kPortFind}, p.data(), p.size());
                // (and loopback, for a second copy of the game on this PC)
                find.send(Addr{0x7F000001u, net::kPortFind}, p.data(), p.size());
            }
            // (entries not heard from in 4 s are gone)
            servers.erase(std::remove_if(servers.begin(), servers.end(),
                                         [&](const ServerEntry& s) { return now - s.seen_ms > (s.invite ? 60000 : 4000); }),
                          servers.end());
            friends.erase(std::remove_if(friends.begin(), friends.end(), [&](const FriendEntry& f) { return now - f.seen_ms > 4000; }),
                          friends.end());
        }
        if (role == Role::None) return;
        // connection upkeep
        for (auto& c : conns) {
            if (!c.used) continue;
            if (now - c.last_recv_ms > kTimeoutMs) {
                drop(c, "connection lost");
                if (role == Role::None) return;
                continue;
            }
            for (auto& p : c.unacked) {
                const int64_t wait = std::max<int64_t>(60, c.rtt > 0 ? c.rtt * 3 / 2 + 15 : 120);
                if (now - p.sent_ms >= wait) {
                    send_raw(c, kReliable, p.seq, p.msg);
                    p.sent_ms = now;
                    p.sends++;
                }
            }
            if (c.ack_due && now - c.last_send_ms >= 15) send_raw(c, kAckOnly, 0, {});
        }
        for (auto it = world_out.begin(); it != world_out.end();) {
            Conn* c = conn_of_slot(it->to);
            if (!c) {
                it = world_out.erase(it);
                continue;
            }
            int room = 48 - (int)c->unacked.size();
            if (it->file[0].empty() && it->file[1].empty()) {   // (no save: the guest starts a new game)
                Writer w;
                w.u8(M_WORLD_PIECE);
                w.u32(it->xfer), w.u8(2), w.u32(0), w.u32(0), w.u16(0);
                send_reliable(*c, w.buf);
                it = world_out.erase(it);
                continue;
            }
            while (room-- > 0 && it->f < 2) {
                const std::vector<uint8_t>& file = it->file[it->f];
                const uint32_t n = std::min<uint32_t>(1024, (uint32_t)file.size() - it->at);
                Writer w;
                w.u8(M_WORLD_PIECE);
                w.u32(it->xfer), w.u8((uint8_t)it->f), w.u32((uint32_t)file.size()), w.u32(it->at), w.u16((uint16_t)n);
                w.raw(file.data() + it->at, n);
                send_reliable(*c, w.buf);
                it->at += n;
                if (it->at >= file.size()) it->f++, it->at = 0;
            }
            if (it->f >= 2) {
                log("session: the host's world sent to %s", players[it->to].name.c_str());
                it = world_out.erase(it);
            } else {
                ++it;
            }
        }
        if (role == Role::Guest && phase == Phase::Connecting && now - join_started_ms > 6000) {
            add_toast("No answer from the host", 2);
            leave();
            return;
        }
        if (now >= next_ping) {
            next_ping = now + 1000;
            Writer w;
            w.u8(M_PING);
            w.u32((uint32_t)now);
            to_all(w.buf, false);
        }
        if (role == Role::Host && now >= next_lobby) {
            next_lobby = now + 500;
            to_all(msg_lobby(), true);
        }
        // Link's state: sent as soon as the game hands a new one (at most every 8 ms)
        if (local_new && me >= 0 && now - last_state_send >= 8) {
            local_new = false;
            last_state_send = now;
            players[me].prev = players[me].st;
            players[me].st = local;
            players[me].st.got_ms = now;
            to_all(msg_state(me, local), false);
        }
        // what Link wears: when it changes (reliable), and every 5 s for players who came since (unreliable)
        if (me >= 0 && phase == Phase::Playing && !local_gear.empty() && (gear_new || now - last_gear_send >= 5000)) {
            Writer w;
            w.u8(M_GEAR);
            w.u8((uint8_t)me);
            w.str(local_gear);
            to_all(w.buf, gear_new);
            gear_new = false;
            last_gear_send = now;
            players[me].gear = local_gear;
        }
        // Link's pose: after the state, at most every 12 ms
        if (pose_new && me >= 0 && phase == Phase::Playing && now - last_pose_send >= 12) {
            pose_new = false;
            last_pose_send = now;
            Writer w;
            w.u8(M_POSE);
            w.u8((uint8_t)me);
            w.u32(++pose_seq);
            w.u16((uint16_t)local_pose.size());
            w.raw(local_pose.data(), local_pose.size());
            to_all(w.buf, false);
        }
    }

    void on_discovery(const Addr& from, const uint8_t* data, int n) {
        if (n < kHeader + 1) return;
        Reader r(data + kHeader + 1, (size_t)n - kHeader - 1);
        const uint8_t type = data[kHeader];
        const int64_t now = net::now_ms();
        if (type == M_BEACON) {
            ServerEntry s;
            s.server = r.str(), s.host = r.str();
            s.players = r.u8(), s.max = r.u8();
            s.in_game = r.u8(), s.locked = r.u8();
            const uint16_t port = r.u16();
            s.session = r.u32();
            if (!r.ok() || (role == Role::Host && s.session == session)) return;
            s.addr = Addr{from.ip, port};
            s.seen_ms = now;
            for (auto& e : servers)
                if (e.session == s.session || e.addr == s.addr) {
                    const bool inv = e.invite;
                    e = s;
                    e.invite = inv;
                    return;
                }
            servers.push_back(s);
        } else if (type == M_PRESENCE) {
            FriendEntry f;
            f.name = r.str();
            f.busy = r.u8();
            if (!r.ok()) return;
            f.find = from;
            f.seen_ms = now;
            if (from.port == find.port() && from.ip == net::preferred_ip() && f.name == name) return;   // (our own)
            for (auto& e : friends)
                if (e.find == f.find) { e = f; return; }
            friends.push_back(f);
        } else if (type == M_INVITE) {
            ServerEntry s;
            s.server = r.str(), s.host = r.str();
            const uint16_t port = r.u16();
            s.session = r.u32();
            if (!r.ok()) return;
            s.addr = Addr{from.ip, port};
            s.invite = true;
            s.seen_ms = now;
            bool found = false;
            for (auto& e : servers)
                if (e.session == s.session) { e.invite = true, e.seen_ms = now, found = true; }
            if (!found) servers.push_back(s);
            add_toast(s.host + " invited you to \"" + s.server + "\"", 1);
        }
    }

    void loop() {
        uint8_t buf[2048];
        while (running) {
            Addr from;
            bool any = false;
            {
                // (the play socket waited on briefly; nothing here holds the lock while waiting)
                int n = play.is_open() ? play.recv(&from, buf, sizeof buf, 0) : 0;
                std::lock_guard<std::mutex> lock(m);
                while (n > 0) {
                    any = true;
                    on_packet(from, buf, n);
                    n = play.is_open() ? play.recv(&from, buf, sizeof buf, 0) : 0;
                }
                int k = find.is_open() ? find.recv(&from, buf, sizeof buf, 0) : 0;
                while (k > 0) {
                    if (k >= kHeader && *(uint32_t*)buf == net::kMagic && buf[6] == kDiscovery) on_discovery(from, buf, k);
                    k = find.recv(&from, buf, sizeof buf, 0);
                }
                tick(net::now_ms());
            }
            if (!any) {
                // (a short wait on the play socket: the next datagram is handled the moment it arrives)
                if (play.is_open()) {
                    int n = play.recv(&from, buf, sizeof buf, 2);
                    if (n > 0) {
                        std::lock_guard<std::mutex> lock(m);
                        on_packet(from, buf, n);
                    }
                } else {
                    // (no session: the discovery socket alone, looked at 50 times a second)
                    Sleep(searching || role != Role::None ? 2 : 20);
                }
            }
        }
    }
};

Session& Session::get() {
    static Session s;
    return s;
}

Session::Session() : d(new Impl) {
    net::startup();
    d->find.open(net::kPortFind, true, true);
    const Config& cfg = config();
    d->name = cfg.name.empty() ? "Link" : cfg.name;
    d->hs = cfg.host;
    d->gs = cfg.guest;
    d->th = std::thread([this] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        d->loop();
    });
    d->th.detach();
}

void Session::set_name(const std::string& n) {
    std::lock_guard<std::mutex> lock(d->m);
    d->name = n.substr(0, 24);
    if (d->me >= 0) d->players[d->me].name = d->name;
}
std::string Session::name() {
    std::lock_guard<std::mutex> lock(d->m);
    return d->name;
}
bool Session::host_start(const HostSettings& s, int file) {
    std::lock_guard<std::mutex> lock(d->m);
    if (d->role != Role::None) d->leave();
    return d->host_start(s, file);
}
void Session::host_stop() {
    std::lock_guard<std::mutex> lock(d->m);
    d->host_stop();
}
bool Session::host_resume() {
    std::lock_guard<std::mutex> lock(d->m);
    return d->host_resume();
}
bool Session::join(const net::Addr& a, const std::string& pw) {
    std::lock_guard<std::mutex> lock(d->m);
    if (d->role != Role::None) d->leave();
    return d->join(a, pw);
}
void Session::leave() {
    std::lock_guard<std::mutex> lock(d->m);
    d->leave();
}
void Session::set_ready(bool ready) {
    std::lock_guard<std::mutex> lock(d->m);
    if (d->me < 0) return;
    d->players[d->me].ready = ready;
    if (d->role == Role::Guest && d->conns[0].used) {
        Writer w;
        w.u8(M_READY);
        w.u8(ready);
        d->send_reliable(d->conns[0], w.buf);
    }
}
void Session::start_game() {
    std::lock_guard<std::mutex> lock(d->m);
    if (d->role != Role::Host) return;
    d->phase = Phase::Starting;
    Writer w;
    w.u8(M_START);
    w.i32(d->host_file == -2 ? -2 : 0);   // (-2: a new game: the guests start one too; else they ask for the host's save)
    d->to_all(w.buf, true);
    d->act({GameAction::StartGame});
    d->next_lobby = 0;
    log("session: the host started the game");
}
void Session::invite(const FriendEntry& f) {
    std::lock_guard<std::mutex> lock(d->m);
    if (d->role != Role::Host || !d->server_running) return;
    Writer w;
    w.u8(M_INVITE);
    w.str(d->hs.server), w.str(d->name);
    w.u16(d->play.port());
    w.u32(d->session);
    auto p = d->packet(kDiscovery, 0, 0, w.buf);
    for (int k = 0; k < 3; k++) d->find.send(f.find, p.data(), p.size());
    d->add_toast("Invitation sent to " + f.name, 1);
}
void Session::kick(int slot) {
    std::lock_guard<std::mutex> lock(d->m);
    if (d->role != Role::Host || slot <= 0 || slot >= kMaxPlayers) return;
    if (Conn* c = d->conn_of_slot(slot)) {
        Writer w;
        w.u8(M_KICK);
        w.str("The host removed you from the session");
        d->send_reliable(*c, w.buf);
        d->flush_some();
        d->drop(*c, "removed");
    }
}
void Session::searching(bool on) {
    std::lock_guard<std::mutex> lock(d->m);
    d->searching = on;
    if (on) d->next_beacon = 0;
}
void Session::push_state(const PlayerState& st) {
    std::lock_guard<std::mutex> lock(d->m);
    d->local = st;
    d->local.seq = ++d->local_seq;
    d->local_new = true;
    if (d->role != Role::None && d->phase == Phase::Starting && (st.flags & kInPlay)) d->phase = Phase::Playing;
}
bool Session::pop_action(GameAction* out) {
    if (d->n_actions.load(std::memory_order_acquire) <= 0) return false;   // (every frame: no lock when there is nothing)
    std::lock_guard<std::mutex> lock(d->m);
    if (d->actions.empty()) return false;
    *out = std::move(d->actions.front());
    d->actions.pop_front();
    d->n_actions.fetch_sub(1, std::memory_order_release);
    return true;
}
void Session::kill_happened(const KillEvent& kill) {
    std::lock_guard<std::mutex> lock(d->m);
    if (d->role == Role::None || !d->hs.share_kills) return;
    KillEvent k = kill;
    if (!k.id) k.id = random_u32() | 1;
    d->to_all(d->msg_kill(k), true);
    d->kill_log(k);
}
bool Session::states(PlayerState st[kMaxPlayers], PlayerState prev[kMaxPlayers], bool used[kMaxPlayers], int* me) {
    std::lock_guard<std::mutex> lock(d->m);
    if (d->role == Role::None) return false;
    for (int i = 0; i < kMaxPlayers; i++) st[i] = d->players[i].st, prev[i] = d->players[i].prev, used[i] = d->players[i].used;
    *me = d->me;
    return true;
}
void Session::push_pose(const std::vector<uint8_t>& blob) {
    if (blob.empty() || blob.size() > 1800) return;
    std::lock_guard<std::mutex> lock(d->m);
    if (d->role == Role::None) return;
    d->local_pose = blob;
    d->pose_new = true;
}
bool Session::take_pose(int slot, uint32_t have, std::vector<uint8_t>* blob, uint32_t* seq) {
    if (slot < 0 || slot >= kMaxPlayers) return false;
    std::lock_guard<std::mutex> lock(d->m);
    const auto& in = d->poses[slot];
    if (d->role == Role::None || !in.seq || in.seq == have || in.blob.empty()) return false;
    *blob = in.blob;
    *seq = in.seq;
    return true;
}
void Session::send_enemy(const EnemySnap& e) {
    if (e.pose.size() > 1800) return;
    std::lock_guard<std::mutex> lock(d->m);
    if (d->role == Role::None || d->me < 0 || d->phase != Phase::Playing) return;
    Writer w;
    w.u8(M_ENEMY);
    w.u8((uint8_t)d->me);
    w.str(e.map), w.str(e.name);
    w.u32(e.id), w.u8(e.lead ? 1 : 0);
    for (float v : e.home) w.f32(v);
    for (float v : e.pos) w.f32(v);
    for (float v : e.rot) w.f32(v);
    w.u16((uint16_t)e.pose.size());
    w.raw(e.pose.data(), e.pose.size());
    d->to_all(w.buf, false);
}
std::vector<EnemySnap> Session::enemies() {
    std::lock_guard<std::mutex> lock(d->m);
    std::vector<EnemySnap> out;
    const int64_t now = net::now_ms();
    for (const auto& e : d->enemy_in)
        if (now - e.got_ms < 400 && e.owner != d->me) out.push_back(e);
    return out;
}
void Session::send_enemy_hit(const std::string& map, const std::string& name, const float home[3], int amount, int owner, uint32_t id) {
    std::lock_guard<std::mutex> lock(d->m);
    if (d->role == Role::None || d->me < 0 || amount <= 0) return;
    Writer w;
    w.u8(M_EHIT);
    w.u8((uint8_t)d->me);
    w.str(map), w.str(name);
    for (int i = 0; i < 3; i++) w.f32(home[i]);
    w.u16((uint16_t)std::min(amount, 65535));
    w.u8(owner >= 0 && owner < kMaxPlayers ? (uint8_t)owner : 0xff), w.u32(id);
    d->to_all(w.buf, true);
}
void Session::send_object_hit(const std::string& map, const std::string& name, uint64_t placement, int amount) {
    std::lock_guard<std::mutex> lock(d->m);
    if (d->role == Role::None || d->me < 0 || amount <= 0 || !placement) return;
    Writer w;
    w.u8(M_OBJHIT);
    w.u8((uint8_t)d->me);
    w.str(map), w.str(name);
    w.u64(placement);
    w.u16((uint16_t)std::min(amount, 65535));
    d->to_all(w.buf, true);
}
void Session::push_gear(const std::string& gear) {
    std::lock_guard<std::mutex> lock(d->m);
    if (d->role == Role::None || gear == d->local_gear) return;
    d->local_gear = gear;
    d->gear_new = true;
}
std::string Session::gear(int slot) {
    if (slot < 0 || slot >= kMaxPlayers) return {};
    std::lock_guard<std::mutex> lock(d->m);
    return d->players[slot].gear;
}
void Session::set_starter(const std::vector<std::pair<std::string, int>>& have) {
    std::lock_guard<std::mutex> lock(d->m);
    d->starter = have;
}
bool Session::take_respawn() { return d->respawn_asked.exchange(false); }
void Session::send_world_flags(const std::string& map, const std::vector<WorldFlag>& flags) {
    std::lock_guard<std::mutex> lock(d->m);
    if (d->role == Role::None || d->me < 0 || flags.empty()) return;
    for (size_t at = 0; at < flags.size(); at += 64) {   // (64 a message)
        const size_t n = std::min<size_t>(64, flags.size() - at);
        Writer w;
        w.u8(M_WFLAG);
        w.u8((uint8_t)d->me);
        w.str(map);
        w.u16((uint16_t)n);
        for (size_t i = at; i < at + n; i++) w.u64(flags[i].key), w.u32(flags[i].value);
        d->to_all(w.buf, true);
    }
}
void Session::request_world() {
    std::lock_guard<std::mutex> lock(d->m);
    if (d->role != Role::Guest || !d->conns[0].used) return;
    d->world = Impl::WorldIn{};
    Writer w;
    w.u8(M_WORLD_ASK);
    d->send_reliable(d->conns[0], w.buf);
}
bool Session::take_world_request(int* slot) {
    std::lock_guard<std::mutex> lock(d->m);
    while (d->role == Role::Host && !d->world_asks.empty()) {
        *slot = d->world_asks.front();
        d->world_asks.erase(d->world_asks.begin());
        if (*slot > 0 && *slot < kMaxPlayers && d->players[*slot].used) return true;
    }
    return false;
}
void Session::send_world(int to, const std::vector<uint8_t>& caption, const std::vector<uint8_t>& progress) {
    std::lock_guard<std::mutex> lock(d->m);
    if (d->role != Role::Host || to <= 0 || to >= kMaxPlayers || !d->players[to].used) return;
    Impl::WorldOut o;
    o.to = to;
    o.xfer = random_u32();
    if (!caption.empty() && !progress.empty()) o.file[0] = caption, o.file[1] = progress;
    d->world_out.push_back(std::move(o));
    log("session: the host's world (%zu + %zu bytes) on its way to %s", caption.size(), progress.size(), d->players[to].name.c_str());
}
int Session::take_world(std::vector<uint8_t>* caption, std::vector<uint8_t>* progress, int* percent) {
    std::lock_guard<std::mutex> lock(d->m);
    auto& w = d->world;
    if (w.none) {
        w = Impl::WorldIn{};
        return -1;
    }
    const uint64_t total = (uint64_t)w.total[0] + w.total[1], have = (uint64_t)w.have[0] + w.have[1];
    if (percent) *percent = total && w.sized[1] ? (int)(have * 100 / total) : 0;
    if (!w.sized[0] || !w.sized[1] || w.have[0] < w.total[0] || w.have[1] < w.total[1]) return 0;
    *caption = std::move(w.file[0]), *progress = std::move(w.file[1]);
    w = Impl::WorldIn{};
    return 1;
}
void Session::send_items(const std::string& list) {
    std::lock_guard<std::mutex> lock(d->m);
    if (d->role != Role::Guest || !d->conns[0].used) return;
    Writer w;
    w.u8(M_ITEMS_KEEP);
    w.str(list);
    d->send_reliable(d->conns[0], w.buf);
}
bool Session::take_items_kept(std::string* name, std::string* list) {
    std::lock_guard<std::mutex> lock(d->m);
    if (d->items_kept.empty()) return false;
    *name = d->items_kept.front().first, *list = d->items_kept.front().second;
    d->items_kept.erase(d->items_kept.begin());
    return true;
}
void Session::send_items_back(int to, const std::string& list) {
    std::lock_guard<std::mutex> lock(d->m);
    if (d->role != Role::Host) return;
    if (Conn* c = d->conn_of_slot(to)) {
        Writer w;
        w.u8(M_ITEMS_BACK);
        w.str(list);
        d->send_reliable(*c, w.buf);
    }
}
bool Session::take_items_back(std::string* list) {
    std::lock_guard<std::mutex> lock(d->m);
    if (!d->items_back_new) return false;
    d->items_back_new = false;
    *list = d->items_back;
    return true;
}
std::vector<WorldFlag> Session::take_world_flags() {
    std::lock_guard<std::mutex> lock(d->m);
    std::vector<WorldFlag> out;
    out.swap(d->world_in);
    return out;
}
std::vector<KillEvent> Session::take_kills() {
    if (d->n_new_kills.load(std::memory_order_acquire) <= 0) return {};
    std::lock_guard<std::mutex> lock(d->m);
    std::vector<KillEvent> out;
    out.swap(d->new_kills);
    d->n_new_kills = 0;
    return out;
}
void Session::revive(int slot) {
    std::lock_guard<std::mutex> lock(d->m);
    if (d->role == Role::None || d->me < 0 || slot < 0 || slot >= kMaxPlayers || slot == d->me || !d->players[slot].used) return;
    Writer w;
    w.u8(M_REVIVE);
    w.u8((uint8_t)slot), w.u8((uint8_t)d->me);
    if (Conn* c = d->route_to(slot)) d->send_reliable(*c, w.buf);
    d->add_toast("You revived " + d->players[slot].name, 1);
}
uint32_t Session::gift_offer(int to, const std::string& item, const std::string& label, int have) {
    std::lock_guard<std::mutex> lock(d->m);
    if (d->role == Role::None || d->me < 0 || to == d->me || to < 0 || to >= kMaxPlayers || !d->players[to].used) return 0;
    Gift g;
    g.id = random_u32() | 1;
    g.from = d->me, g.to = to, g.item = item, g.label = label, g.have = have, g.state = kGiftOffered;
    g.at_ms = net::now_ms();
    d->gifts[g.id] = g;
    if (Conn* c = d->route_to(to)) d->send_reliable(*c, d->msg_gift(M_GIFT_OFFER, g));
    d->add_toast("Offer sent to " + d->players[to].name, 0);
    return g.id;
}
void Session::gift_reply(uint32_t id, bool accept) {
    std::lock_guard<std::mutex> lock(d->m);
    d->gift_reply(id, accept);
}
void Session::gift_send(uint32_t id, int amount) {
    std::lock_guard<std::mutex> lock(d->m);
    auto it = d->gifts.find(id);
    if (it == d->gifts.end()) return;
    Gift& g = it->second;
    g.amount = amount;
    g.state = kGiftSent;
    // (taken from this game first; sent once the game says it is gone: gift_result)
    d->act({GameAction::RemoveItem, g.item, amount, id});
}
void Session::gift_cancel(uint32_t id) {
    std::lock_guard<std::mutex> lock(d->m);
    auto it = d->gifts.find(id);
    if (it == d->gifts.end()) return;
    if (it->second.from == d->me && it->second.state == kGiftAccepted) {
        it->second.state = kGiftFailed;
        it->second.amount = 0;
        if (Conn* c = d->route_to(it->second.to)) d->send_reliable(*c, d->msg_gift(M_GIFT_DONE, it->second));
    }
    d->gifts.erase(it);
}
std::vector<Gift> Session::gifts() {
    std::lock_guard<std::mutex> lock(d->m);
    std::vector<Gift> out;
    const int64_t now = net::now_ms();
    for (auto it = d->gifts.begin(); it != d->gifts.end();) {
        if ((it->second.state == kGiftDone || it->second.state == kGiftFailed || it->second.state == kGiftDeclined) &&
            now - it->second.at_ms > 8000)
            it = d->gifts.erase(it);
        else
            out.push_back((it++)->second);
    }
    return out;
}
void Session::gift_result(uint32_t id, bool ok, int moved) {
    std::lock_guard<std::mutex> lock(d->m);
    auto it = d->gifts.find(id);
    if (it == d->gifts.end()) return;
    Gift& g = it->second;
    g.at_ms = net::now_ms();
    if (g.from == d->me) {
        // (the giver's game took the items: as many as it really took go)
        if (!ok || moved <= 0) {
            g.state = kGiftFailed;
            g.amount = 0;
            d->add_toast("The items could not be taken from your bag", 2);
            if (Conn* c = d->route_to(g.to)) d->send_reliable(*c, d->msg_gift(M_GIFT_DONE, g));   // (nothing comes)
            return;
        }
        g.amount = moved;
        if (Conn* c = d->route_to(g.to)) d->send_reliable(*c, d->msg_gift(M_GIFT_SEND, g));
    } else {
        // (the receiver's game took them: as many as it could hold; the giver gets the rest back)
        const std::string from = g.from >= 0 && g.from < kMaxPlayers ? d->players[g.from].name : "a player";
        const int sent = g.amount;
        g.state = ok ? kGiftDone : kGiftFailed;
        g.amount = std::max(0, moved);
        if (ok && moved < sent)
            d->add_toast("Received " + std::to_string(moved) + " of " + std::to_string(sent) + " " + g.label + " from " + from +
                             " (no room for the rest)",
                         1);
        else if (ok)
            d->add_toast("Received " + std::to_string(moved) + "x " + g.label + " from " + from, 1);
        else
            d->add_toast("You cannot carry " + g.label + " yet: it went back to " + from, 2);
        if (Conn* c = d->route_to(g.from)) d->send_reliable(*c, d->msg_gift(M_GIFT_DONE, g));
    }
}
SessionView Session::view() {
    std::lock_guard<std::mutex> lock(d->m);
    SessionView v;
    v.role = d->role, v.phase = d->phase, v.me = d->me;
    for (int i = 0; i < kMaxPlayers; i++) v.players[i] = d->players[i];
    v.server = d->role == Role::Guest ? d->server_name : d->hs.server;
    v.host_ip = d->host_ip, v.status = d->status, v.server_running = d->server_running, v.host_file = d->host_file;
    v.host = d->hs;
    return v;
}
std::vector<ServerEntry> Session::servers() {
    std::lock_guard<std::mutex> lock(d->m);
    return d->servers;
}
std::vector<FriendEntry> Session::friends() {
    std::lock_guard<std::mutex> lock(d->m);
    return d->friends;
}
std::vector<Toast> Session::toasts() {
    std::lock_guard<std::mutex> lock(d->m);
    return std::vector<Toast>(d->toasts.begin(), d->toasts.end());
}
void Session::toast(const std::string& text, int kind) {
    std::lock_guard<std::mutex> lock(d->m);
    d->add_toast(text, kind);
}
std::vector<KillEvent> Session::kills_for(const std::string& map) {
    std::lock_guard<std::mutex> lock(d->m);
    std::vector<KillEvent> out;
    for (const auto& k : d->kills)
        if (k.map == map) out.push_back(k);
    return out;
}
HostSettings Session::host_settings() {
    std::lock_guard<std::mutex> lock(d->m);
    return d->hs;
}
void Session::set_host_settings(const HostSettings& s) {
    std::lock_guard<std::mutex> lock(d->m);
    d->hs = s;
    d->next_lobby = 0;
}
GuestSettings Session::guest_settings() {
    std::lock_guard<std::mutex> lock(d->m);
    return d->gs;
}
void Session::set_guest_settings(const GuestSettings& s) {
    std::lock_guard<std::mutex> lock(d->m);
    d->gs = s;
}
bool Session::others_present() {
    std::lock_guard<std::mutex> lock(d->m);
    if (d->role == Role::None) return false;
    for (int i = 0; i < kMaxPlayers; i++)
        if (i != d->me && d->players[i].used) return true;
    return false;
}
int64_t Session::last_toast_ms() {
    std::lock_guard<std::mutex> lock(d->m);
    return d->toasts.empty() ? 0 : d->toasts.back().at_ms;
}
bool Session::active() {
    std::lock_guard<std::mutex> lock(d->m);
    return d->role != Role::None;
}

}  // namespace coop
