// Longshot Co-op networking: UDP over the players' shared network (a Radmin VPN network: 26.x.x.x, or any LAN).
//
// One socket per game for play (kPortPlay by default: settings) and one for discovery (kPortFind, broadcast). The host is
// the hub: guests talk to it, it relays what one guest sends for the others. Each datagram carries a header with a
// sequence number and an acknowledgement window, so reliable messages (lobby, events, give, kills) are resent until acked
// and handed over in order, while the frequent player states go unreliable and only the newest counts. A network thread of
// its own does all socket work: the game's threads never wait on the network.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace coop::net {

constexpr uint16_t kPortPlay = 47812;
constexpr uint16_t kPortFind = 47813;
constexpr uint32_t kMagic = 0x4F43534C;   // "LSCO"
constexpr uint16_t kProtocol = 7;

struct Addr {
    uint32_t ip = 0;     // host order
    uint16_t port = 0;
    bool operator==(const Addr& o) const { return ip == o.ip && port == o.port; }
    bool operator!=(const Addr& o) const { return !(*this == o); }
    std::string str() const;
    bool valid() const { return ip != 0 && port != 0; }
};

// a little-endian message writer / reader
class Writer {
public:
    std::vector<uint8_t> buf;
    void u8(uint8_t v) { buf.push_back(v); }
    void u16(uint16_t v) { raw(&v, 2); }
    void u32(uint32_t v) { raw(&v, 4); }
    void u64(uint64_t v) { raw(&v, 8); }
    void i32(int32_t v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }
    void str(const std::string& s) {
        const uint16_t n = (uint16_t)(s.size() > 1000 ? 1000 : s.size());
        u16(n);
        raw(s.data(), n);
    }
    void raw(const void* p, size_t n) { buf.insert(buf.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool ok() const { return ok_; }
    size_t left() const { return n_ - at_; }
    uint8_t u8() { uint8_t v = 0; raw(&v, 1); return v; }
    uint16_t u16() { uint16_t v = 0; raw(&v, 2); return v; }
    uint32_t u32() { uint32_t v = 0; raw(&v, 4); return v; }
    uint64_t u64() { uint64_t v = 0; raw(&v, 8); return v; }
    int32_t i32() { int32_t v = 0; raw(&v, 4); return v; }
    float f32() { float v = 0; raw(&v, 4); return v; }
    std::string str() {
        const uint16_t n = u16();
        if (!ok_ || n > left()) { ok_ = false; return {}; }
        std::string s((const char*)p_ + at_, n);
        at_ += n;
        return s;
    }
    void raw(void* dst, size_t n) {
        if (!ok_ || n > left()) { ok_ = false; memset(dst, 0, n); return; }
        memcpy(dst, p_ + at_, n);
        at_ += n;
    }

private:
    const uint8_t* p_;
    size_t n_, at_ = 0;
    bool ok_ = true;
};

// The local IPv4 interfaces (up, not loopback): their address and broadcast; radmin: the Radmin VPN adapter's
struct Iface {
    uint32_t ip = 0, mask = 0, bcast = 0;
    std::string name;
    bool radmin = false;
};
std::vector<Iface> interfaces();
// the address others on the shared network reach this PC at: the Radmin VPN adapter's first, else the first LAN's
uint32_t preferred_ip();

// ---- the sockets (used by the session's network thread only)
class Socket {
public:
    bool open(uint16_t port, bool broadcast, bool reuse);   // bound to every interface; port 0: any
    void close();
    bool send(const Addr& to, const void* data, size_t n);
    // waits up to timeout_ms for a datagram: its size (0 none, -1 error)
    int recv(Addr* from, void* buf, size_t cap, int timeout_ms);
    uint16_t port() const { return port_; }
    bool is_open() const { return s_ != ~0ull; }

private:
    uint64_t s_ = ~0ull;
    uint16_t port_ = 0;
};

bool startup();   // WSAStartup once
int64_t now_ms();

}  // namespace coop::net
