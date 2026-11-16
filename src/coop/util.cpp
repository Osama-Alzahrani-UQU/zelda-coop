#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "common.h"
#include "runtime.h"

namespace coop {

namespace {
std::mutex log_mutex;
FILE* log_file = nullptr;
std::wstring g_bin, g_game, g_mod;

std::wstring parent(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(0, s);
}

void init_dirs() {
    if (!g_bin.empty()) return;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    g_bin = parent(path);
    // (bin\longshot.exe beside image.bin's folder: the game folder is bin's parent, as host/hos_paths.h decides)
    std::wstring up = parent(g_bin);
    g_game = GetFileAttributesW((up + L"\\image.bin").c_str()) != INVALID_FILE_ATTRIBUTES ? up : g_bin;
    g_mod = g_bin + L"\\coop";
}
}  // namespace

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// (LONGSHOT_COOP_PROFILE=name: a second copy of the game from the same folder keeps its own settings and log: tests)
std::wstring profile_suffix() {
    wchar_t v[32];
    const DWORD n = GetEnvironmentVariableW(L"LONGSHOT_COOP_PROFILE", v, 32);
    return n && n < 32 ? std::wstring(L"_") + v : std::wstring();
}

const std::wstring& bin_dir() { init_dirs(); return g_bin; }
const std::wstring& game_dir() { init_dirs(); return g_game; }
const std::wstring& mod_dir() { init_dirs(); return g_mod; }

int64_t now_ns() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (int64_t)((double)t.QuadPart * 1e9 / (double)freq.QuadPart);
}

std::string cstr_at(uint64_t a, size_t max) {
    std::string s;
    for (size_t i = 0; i < max; i++) {
        char ch = (char)r8(a + i);
        if (!ch) break;
        s.push_back(ch);
    }
    return s;
}

void log(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    {
        std::lock_guard<std::mutex> lock(log_mutex);
        if (!log_file) {
            init_dirs();
            CreateDirectoryW(g_mod.c_str(), nullptr);
            log_file = _wfopen((g_mod + L"\\coop_log" + profile_suffix() + L".txt").c_str(), L"w");
        }
        if (log_file) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(log_file, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
            fflush(log_file);
        }
    }
    // (the game's own log as well, so a bug report from the launcher carries the mod's lines)
    if (rt().hos_log) rt().hos_log("coop: %s", line);
}

void log_flush() {
    std::lock_guard<std::mutex> lock(log_mutex);
    if (log_file) fflush(log_file);
}

std::string place_name(const std::string& map) {
    std::string m = map;
    const size_t slash = m.rfind('/');
    if (slash != std::string::npos) m = m.substr(slash + 1);
    for (const char* suffix : {"_Child", "_Adult"}) {
        const size_t n = strlen(suffix);
        if (m.size() > n && m.compare(m.size() - n, n, suffix) == 0) m.resize(m.size() - n);
    }
    std::string out;
    for (size_t i = 0; i < m.size(); i++) {
        const char ch = m[i] == '_' ? ' ' : m[i];
        if (i > 0 && ch >= 'A' && ch <= 'Z' && m[i - 1] >= 'a' && m[i - 1] <= 'z') out.push_back(' ');
        if (ch != ' ' || (!out.empty() && out.back() != ' ')) out.push_back(ch);
    }
    return out.empty() ? map : out;
}

}  // namespace coop
