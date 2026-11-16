#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <mutex>

#include "net.h"

namespace coop::net {

std::string Addr::str() const {
    char b[32];
    snprintf(b, sizeof b, "%u.%u.%u.%u:%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port);
    return b;
}

bool startup() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA w;
        ok = WSAStartup(MAKEWORD(2, 2), &w) == 0;
    });
    return ok;
}

int64_t now_ms() { return (int64_t)GetTickCount64(); }

std::vector<Iface> interfaces() {
    std::vector<Iface> out;
    ULONG size = 32 * 1024;
    std::vector<uint8_t> buf(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    }
    if (r != NO_ERROR) return out;
    for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        char name[256] = {0};
        WideCharToMultiByte(CP_UTF8, 0, a->Description ? a->Description : L"", -1, name, sizeof name, nullptr, nullptr);
        char fname[256] = {0};
        WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName ? a->FriendlyName : L"", -1, fname, sizeof fname, nullptr, nullptr);
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* sin = (const sockaddr_in*)u->Address.lpSockaddr;
            Iface f;
            f.ip = ntohl(sin->sin_addr.s_addr);
            const ULONG bits = u->OnLinkPrefixLength;
            f.mask = bits == 0 ? 0 : bits >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - bits)) - 1);
            f.bcast = (f.ip & f.mask) | ~f.mask;
            f.name = fname;
            f.radmin = strstr(name, "Radmin") != nullptr || strstr(fname, "Radmin") != nullptr;
            if ((f.ip >> 16) == 0xA9FE) continue;   // (169.254: no network behind it)
            out.push_back(f);
        }
    }
    return out;
}

uint32_t preferred_ip() {
    const auto ifs = interfaces();
    for (const auto& f : ifs)
        if (f.radmin) return f.ip;
    for (const auto& f : ifs)
        if ((f.ip >> 24) == 26) return f.ip;   // (Radmin's network is 26.0.0.0/8)
    return ifs.empty() ? 0 : ifs[0].ip;
}

bool Socket::open(uint16_t port, bool broadcast, bool reuse) {
    close();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    BOOL on = TRUE;
    if (broadcast) setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&on, sizeof on);
    if (reuse) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
    int bufsz = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&bufsz, sizeof bufsz);
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&bufsz, sizeof bufsz);
    // (an ICMP port-unreachable from a peer that left would fail every later recvfrom with WSAECONNRESET)
    DWORD no = FALSE, ret = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &no, sizeof no, nullptr, 0, &ret, nullptr, nullptr);
    // (low latency: the datagrams marked as interactive)
    int tos = 0xB8;   // (DSCP EF)
    setsockopt(s, IPPROTO_IP, IP_TOS, (const char*)&tos, sizeof tos);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, (sockaddr*)&a, sizeof a) != 0) {
        closesocket(s);
        return false;
    }
    int len = sizeof a;
    getsockname(s, (sockaddr*)&a, &len);
    port_ = ntohs(a.sin_port);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    s_ = (uint64_t)s;
    return true;
}

void Socket::close() {
    if (s_ != ~0ull) closesocket((SOCKET)s_);
    s_ = ~0ull;
    port_ = 0;
}

bool Socket::send(const Addr& to, const void* data, size_t n) {
    if (s_ == ~0ull || !to.valid()) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(to.ip);
    a.sin_port = htons(to.port);
    return sendto((SOCKET)s_, (const char*)data, (int)n, 0, (sockaddr*)&a, sizeof a) == (int)n;
}

int Socket::recv(Addr* from, void* buf, size_t cap, int timeout_ms) {
    if (s_ == ~0ull) return -1;
    sockaddr_in a{};
    int len = sizeof a;
    int n = recvfrom((SOCKET)s_, (char*)buf, (int)cap, 0, (sockaddr*)&a, &len);
    if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK && timeout_ms > 0) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET((SOCKET)s_, &rs);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        if (select(0, &rs, nullptr, nullptr, &tv) <= 0) return 0;
        len = sizeof a;
        n = recvfrom((SOCKET)s_, (char*)buf, (int)cap, 0, (sockaddr*)&a, &len);
    }
    if (n < 0) return WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAECONNRESET ? 0 : -1;
    from->ip = ntohl(a.sin_addr.s_addr);
    from->port = ntohs(a.sin_port);
    return n;
}

}  // namespace coop::net
